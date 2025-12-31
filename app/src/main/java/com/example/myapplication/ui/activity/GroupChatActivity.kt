package com.example.myapplication.ui.activity

import android.os.Bundle
import android.util.Log
import android.view.View
import android.view.ViewGroup
import android.widget.ImageView
import android.app.AlertDialog
import android.content.Intent
import android.widget.EditText
import android.widget.ImageButton
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.Observer
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.example.myapplication.R
import com.example.myapplication.data.database.ChatDatabase
import com.example.myapplication.data.model.GroupMember
import com.example.myapplication.network.NetworkManager
import com.example.myapplication.ui.adapter.GroupMessage
import com.example.myapplication.ui.adapter.GroupMessageAdapter
import com.example.myapplication.data.repository.SessionManager
import androidx.activity.viewModels
import com.example.myapplication.ui.viewmodel.ChatViewModel
import com.google.android.material.appbar.MaterialToolbar

class GroupChatActivity : AppCompatActivity() {
    private lateinit var networkManager: NetworkManager
    private lateinit var sessionManager: SessionManager
    private lateinit var recyclerView: RecyclerView
    private lateinit var etMessage: EditText
    private lateinit var btnSend: ImageButton
    private lateinit var adapter: GroupMessageAdapter
    private val messagesList = mutableListOf<GroupMessage>()
    
    private var groupId: Int = 0
    private var groupName: String = ""
    private var myUserId: Int = 0
    private var myUsername: String = ""
    private var historyLoaded: Boolean = false
    private val chatViewModel: ChatViewModel by viewModels()
    
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_group_chat)
        
        networkManager = NetworkManager.getInstance()
        sessionManager = SessionManager.getInstance(this)
        
        groupId = intent.getIntExtra("groupId", 0)
        groupName = intent.getStringExtra("groupName") ?: "Group"
        myUserId = sessionManager.getUserId()
        myUsername = sessionManager.getUsername() ?: "Me"
        
        val toolbar = findViewById<MaterialToolbar>(R.id.toolbar)
        toolbar.title = groupName
        setSupportActionBar(toolbar)
        supportActionBar?.setDisplayHomeAsUpEnabled(true)
        
        recyclerView = findViewById(R.id.recyclerView)
        etMessage = findViewById(R.id.etMessage)
        btnSend = findViewById(R.id.btnSend)
        
        adapter = GroupMessageAdapter(messagesList)
        recyclerView.adapter = adapter
        recyclerView.layoutManager = LinearLayoutManager(this)
        
        val db = ChatDatabase.getDatabase(this)
        // Update toolbar subtitle with member count instead of a removed tvMemberCount view
        db.groupMemberDao().getGroupMembers(groupId).observe(this, Observer { members ->
            val tb = findViewById<com.google.android.material.appbar.MaterialToolbar>(R.id.toolbar)
            tb.subtitle = "${members.size} members"
        })

        // Info button (was member count badge) — opens group options
        val btnInfo = findViewById<TextView>(R.id.btnInfo)
        btnInfo.setOnClickListener {
            showGroupOptions()
        }

        // Load persisted group messages from Room and observe for changes
        db.messageDao().getGroupMessages(groupId).observe(this, Observer { dbMessages ->
            // map DB messages to UI model, resolving sender names from local user DB when possible
            messagesList.clear()

            dbMessages.forEach { m ->
                // Check if this is a system message (senderId = 0)
                if (m.senderId == 0) {
                    // System message — show as centered notification
                    messagesList.add(
                        GroupMessage(
                            senderId = m.senderId,
                            senderName = "",
                            message = m.content,
                            timestamp = m.timestamp,
                            isMine = false,
                            isSystemMessage = true
                        )
                    )
                } else if (m.senderId == myUserId) {
                    // own message — we have the username locally
                    messagesList.add(
                        GroupMessage(
                            senderId = m.senderId,
                            senderName = myUsername,
                            message = m.content,
                            timestamp = m.timestamp,
                            isMine = true,
                            isSystemMessage = false
                        )
                    )
                } else {
                    // other user's message — try to resolve username from local DB asynchronously
                    // add a placeholder now and replace it when the username is available
                    val placeholder = GroupMessage(
                        senderId = m.senderId,
                        senderName = "User ${m.senderId}",
                        message = m.content,
                        timestamp = m.timestamp,
                        isMine = false,
                        isSystemMessage = false
                    )
                    val index = messagesList.size
                    messagesList.add(placeholder)

                    lifecycleScope.launch(Dispatchers.IO) {
                        try {
                            // Try to resolve from group members table first
                            val gm = db.groupMemberDao().getMember(groupId, m.senderId)
                            val resolvedName = when {
                                gm != null && !gm.nickname.isNullOrEmpty() -> gm.nickname
                                gm != null && !gm.username.startsWith("User ") -> gm.username
                                else -> {
                                    // fallback to friend cache, then users table
                                    val friend = db.friendDao().getFriend(m.senderId)
                                    friend?.username ?: db.userDao().getUser(m.senderId)?.username
                                }
                            } ?: "User ${m.senderId}"

                            launch(Dispatchers.Main) {
                                // replace placeholder with resolved name
                                messagesList[index] = placeholder.copy(senderName = resolvedName)
                                adapter.notifyItemChanged(index)
                            }
                        } catch (e: Exception) {
                            // ignore and keep placeholder
                        }
                    }
                }
            }

            adapter.notifyDataSetChanged()
            if (messagesList.isNotEmpty()) recyclerView.scrollToPosition(messagesList.size - 1)
        })
        
        // Always load history from server on first open to ensure we have latest messages
        if (!historyLoaded && networkManager.isConnected()) {
            historyLoaded = true
            // Clear local messages for this group before loading from server
            lifecycleScope.launch(Dispatchers.IO) {
                try {
                    db.messageDao().deleteGroupMessages(groupId)
                    Log.d("GroupChatActivity", "Cleared local group messages for groupId=$groupId")
                } catch (e: Exception) {
                    Log.e("GroupChatActivity", "Error clearing group messages", e)
                }
            }
            chatViewModel.loadGroupHistory(groupId)
        }
        
        setupNetworkCallback()
        setupSendButton()
    }
    
    private fun setupNetworkCallback() {
        networkManager.setCallback(object : NetworkManager.MessageCallback {
            override fun onMessageReceived(messageType: Int, payload: String) {
                runOnUiThread {
                    when (messageType) {
                        NetworkManager.MessageType.GROUP_MESSAGE -> handleIncomingMessage(payload)
                        NetworkManager.MessageType.SUCCESS -> {
                            // Possibly a group history response — detect pattern
                            try {
                                // Parse payload into map first
                                val pairs = payload.split("&").mapNotNull {
                                    val parts = it.split("=", limit = 2)
                                    if (parts.size == 2) parts[0] to parts[1] else null
                                }.toMap()
                                
                                // Handle group history response
                                if (payload.contains("groupId=") && payload.contains("count=") && payload.contains("from0=") && payload.contains("content0=")) {
                                    val respGroupId = pairs["groupId"]?.toIntOrNull()
                                    if (respGroupId != null && respGroupId == groupId) {
                                        val count = pairs["count"]?.toIntOrNull() ?: 0
                                        // Insert each history message into DB via ViewModel
                                        for (i in 0 until count) {
                                            val fromId = pairs["from$i"]?.toIntOrNull() ?: continue
                                            val content = pairs["content$i"] ?: continue
                                            val ts = pairs["timestamp$i"]?.toLongOrNull() ?: System.currentTimeMillis()
                                            // ensure timestamp is in milliseconds
                                            val tsMillis = if (ts < 2000000000L) ts * 1000 else ts
                                            chatViewModel.receiveMessage(fromId, content, tsMillis, groupId)
                                        }
                                        Toast.makeText(this@GroupChatActivity, "Loaded $count messages from group history", Toast.LENGTH_SHORT).show()
                                    }
                                }
                                
                                // Handle rename notification: update title bar immediately
                                if (pairs["action"] == "renamed" && pairs.containsKey("groupId") && pairs.containsKey("name")) {
                                    val notifGroupId = pairs["groupId"]?.toIntOrNull()
                                    if (notifGroupId == groupId) {
                                        val newName = pairs["name"] ?: ""
                                        if (newName.isNotEmpty()) {
                                            groupName = newName
                                            val toolbar = findViewById<com.google.android.material.appbar.MaterialToolbar>(R.id.toolbar)
                                            toolbar.title = newName
                                            // Also update local DB
                                            lifecycleScope.launch(Dispatchers.IO) {
                                                try {
                                                    val db = ChatDatabase.getDatabase(this@GroupChatActivity)
                                                    val g = db.groupDao().getGroup(groupId)
                                                    if (g != null) {
                                                        db.groupDao().updateGroup(g.copy(name = newName))
                                                    }
                                                } catch (e: Exception) {
                                                    // ignore
                                                }
                                            }
                                        }
                                    }
                                }
                            } catch (e: Exception) {
                                // ignore non-history success responses
                            }
                        }
                        NetworkManager.MessageType.ERROR -> {
                            Toast.makeText(this@GroupChatActivity, "Error: $payload", Toast.LENGTH_SHORT).show()
                        }
                    }
                }
            }
        })
    }
    
    private fun setupSendButton() {
        btnSend.setOnClickListener {
            val message = etMessage.text.toString().trim()
            if (message.isNotEmpty()) {
                sendMessage(message)
                etMessage.text.clear()
            }
        }
    }
    
    private fun sendMessage(message: String) {
        // send via network (optimistic) and persist locally
        val sent = networkManager.sendGroupMessage(groupId, message)

        val timestamp = System.currentTimeMillis()
        val groupMessage = GroupMessage(
            senderId = myUserId,
            senderName = myUsername,
            message = message,
            timestamp = timestamp,
            isMine = true,
            isSystemMessage = false
        )

        // Optimistic UI update
        messagesList.add(groupMessage)
        adapter.notifyItemInserted(messagesList.size - 1)
        recyclerView.scrollToPosition(messagesList.size - 1)

        // Persist to Room so message remains after re-opening the group
        val db = ChatDatabase.getDatabase(this)
        lifecycleScope.launch(Dispatchers.IO) {
            try {
                val msg = com.example.myapplication.data.model.Message(
                    senderId = myUserId,
                    recipientId = -1,
                    groupId = groupId,
                    content = message,
                    timestamp = timestamp,
                    isSent = sent,
                    isDelivered = sent
                )
                db.messageDao().insertMessage(msg)
            } catch (e: Exception) {
                e.printStackTrace()
            }
        }

        if (!sent) {
            Toast.makeText(this, "Failed to send message", Toast.LENGTH_SHORT).show()
        }
    }
    
    private fun handleIncomingMessage(payload: String) {
        val pairs = payload.split("&").mapNotNull {
            val parts = it.split("=", limit = 2)
            if (parts.size == 2) parts[0] to parts[1] else null
        }.toMap()
        
        val msgGroupId = pairs["groupId"]?.toIntOrNull() ?: return
        if (msgGroupId != groupId) return // Not for this group
        
        val senderId = pairs["senderId"]?.toIntOrNull() ?: return
        val message = pairs["message"] ?: return
        val timestamp = pairs["timestamp"]?.toLongOrNull() ?: System.currentTimeMillis()
        
        // Check if this is a system message (senderId = 0)
        val isSystemMessage = (senderId == 0)
        
            // Persist incoming group message to local DB (use ViewModel to reuse dedup logic)
            try {
                // convert timestamp to milliseconds if server sent seconds
                val tsMillis = if (timestamp < 2000000000L) timestamp * 1000 else timestamp
                chatViewModel.receiveMessage(senderId, message, tsMillis, groupId)
            } catch (e: Exception) {
                // ignore persistence errors
            }

        // System messages: always show them as centered notifications
        if (isSystemMessage) {
            val systemMsg = GroupMessage(
                senderId = senderId,
                senderName = "",
                message = message,
                timestamp = timestamp,
                isMine = false,
                isSystemMessage = true
            )
            messagesList.add(systemMsg)
            adapter.notifyItemInserted(messagesList.size - 1)
            recyclerView.scrollToPosition(messagesList.size - 1)
            
            // Parse system message to update local database state
            lifecycleScope.launch(Dispatchers.IO) {
                try {
                    val db = ChatDatabase.getDatabase(this@GroupChatActivity)
                    
                    // Pattern: "X removed Y from the group"
                    if (message.contains(" removed ") && message.contains(" from the group")) {
                        val parts = message.split(" removed ")
                        if (parts.size == 2) {
                            val removedUsername = parts[1].replace(" from the group", "").trim()
                            // Find user ID by username and remove from group
                            val member = db.groupMemberDao().getMembersList(groupId)
                                .find { it.username.equals(removedUsername, ignoreCase = true) }
                            if (member != null) {
                                db.groupMemberDao().removeMember(groupId, member.userId)
                            }
                        }
                    }
                    // Pattern: "X made Y an admin"
                    else if (message.contains(" made ") && message.contains(" an admin")) {
                        val parts = message.split(" made ")
                        if (parts.size == 2) {
                            val adminUsername = parts[1].replace(" an admin", "").trim()
                            // Find user and update admin status
                            val member = db.groupMemberDao().getMembersList(groupId)
                                .find { it.username.equals(adminUsername, ignoreCase = true) }
                            if (member != null) {
                                db.groupMemberDao().updateMember(member.copy(isAdmin = true))
                            }
                        }
                    }
                } catch (e: Exception) {
                    // ignore parsing errors
                }
            }
        } else if (senderId != myUserId) {
            // Don't show our own messages again (already added in sendMessage)
            // resolve sender username from local DB; show placeholder while loading
            val placeholder = GroupMessage(
                senderId = senderId,
                senderName = "User $senderId",
                message = message,
                timestamp = timestamp,
                isMine = false,
                isSystemMessage = false
            )
            val index = messagesList.size
            messagesList.add(placeholder)
            adapter.notifyItemInserted(messagesList.size - 1)
            recyclerView.scrollToPosition(messagesList.size - 1)

            lifecycleScope.launch(Dispatchers.IO) {
                try {
                    val db = ChatDatabase.getDatabase(this@GroupChatActivity)
                    val gm = db.groupMemberDao().getMember(groupId, senderId)
                    val resolved = when {
                        gm != null && !gm.nickname.isNullOrEmpty() -> gm.nickname
                        gm != null && !gm.username.startsWith("User ") -> gm.username
                        else -> {
                            val friend = db.friendDao().getFriend(senderId)
                            friend?.username ?: db.userDao().getUser(senderId)?.username
                        }
                    } ?: "User $senderId"

                    launch(Dispatchers.Main) {
                        messagesList[index] = placeholder.copy(senderName = resolved)
                        adapter.notifyItemChanged(index)
                    }
                } catch (e: Exception) {
                    // ignore and keep placeholder
                }
            }
        }
    }
    
    override fun onSupportNavigateUp(): Boolean {
        finish()
        return true
    }

    override fun onResume() {
        super.onResume()
        // If we didn't load history earlier (e.g. app started offline), try again when resumed and online
        try {
            if (!historyLoaded && networkManager.isConnected()) {
                historyLoaded = true
                chatViewModel.loadGroupHistory(groupId)
            }
        } catch (e: Exception) {
            // ignore
        }
    }

    private fun showGroupOptions() {
        val options = arrayOf("Rename group", "View members", "Nickname", "Leave group")
        AlertDialog.Builder(this)
            .setTitle("Group Options")
            .setItems(options) { _, which ->
                when (which) {
                    0 -> showRenameDialog()
                    1 -> showMembersDialog()
                    2 -> showSetNicknameDialog()
                    3 -> confirmAndLeaveGroup()
                }
            }
            .show()
    }

    private fun showRenameDialog() {
        val input = EditText(this)
        input.setText(groupName)
        AlertDialog.Builder(this)
            .setTitle("Rename Group")
            .setView(input)
            .setPositiveButton("OK") { _, _ ->
                val newName = input.text.toString().trim()
                if (newName.isNotEmpty()) {
                    lifecycleScope.launch {
                        try {
                            // Send rename request to server first
                            val success = networkManager.renameGroup(groupId, newName)
                            if (success) {
                                // Server will broadcast system message and rename notification to all members
                                // Update local DB and UI immediately for responsive feel
                                val db = ChatDatabase.getDatabase(this@GroupChatActivity)
                                val g = db.groupDao().getGroup(groupId)
                                if (g != null) {
                                    db.groupDao().updateGroup(g.copy(name = newName))
                                } else {
                                    db.groupDao().insertGroup(com.example.myapplication.data.model.Group(id = groupId, name = newName, creatorId = myUserId))
                                }
                                groupName = newName
                                val toolbar = findViewById<com.google.android.material.appbar.MaterialToolbar>(R.id.toolbar)
                                toolbar.title = newName
                                Toast.makeText(this@GroupChatActivity, "Group renamed", Toast.LENGTH_SHORT).show()
                            } else {
                                Toast.makeText(this@GroupChatActivity, "Failed to rename group", Toast.LENGTH_SHORT).show()
                            }
                        } catch (e: Exception) {
                            Toast.makeText(this@GroupChatActivity, "Error: ${e.message}", Toast.LENGTH_SHORT).show()
                        }
                    }
                }
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun showMembersDialog() {
        // Show a richer members list matching requested style (avatar + subtitle + add member)
        val dialogView = layoutInflater.inflate(R.layout.dialog_members, null)
        val rv = dialogView.findViewById<androidx.recyclerview.widget.RecyclerView>(R.id.rvMembers)
        rv.layoutManager = androidx.recyclerview.widget.LinearLayoutManager(this)

        val db = ChatDatabase.getDatabase(this)
        val live = db.groupMemberDao().getGroupMembers(groupId)
        lateinit var observer: androidx.lifecycle.Observer<List<GroupMember>>
        observer = androidx.lifecycle.Observer { members ->
            val baseList = (members ?: emptyList()).toMutableList()
            // Add an 'Add member' synthetic item at the end so users can add members from here
            val addEntry = com.example.myapplication.data.model.GroupMember(groupId = groupId, userId = -1, username = "Add members")
            baseList.add(addEntry)

            // Backing list that we can update as names are resolved
            val list = baseList
            // Determine if current user is admin (viewer)
            val viewerIsAdmin = baseList.any { it.userId == sessionManager.getUserId() && it.isAdmin }

            val adapter = object : androidx.recyclerview.widget.RecyclerView.Adapter<androidx.recyclerview.widget.RecyclerView.ViewHolder>() {
                override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): androidx.recyclerview.widget.RecyclerView.ViewHolder {
                    val v = layoutInflater.inflate(R.layout.item_member_view, parent, false)
                    return object : androidx.recyclerview.widget.RecyclerView.ViewHolder(v) {}
                }

                override fun onBindViewHolder(holder: androidx.recyclerview.widget.RecyclerView.ViewHolder, position: Int) {
                    val item = list[position]
                    val v = holder.itemView
                    val tvName = v.findViewById<TextView>(R.id.tvMemberName)
                    val tvSubtitle = v.findViewById<TextView>(R.id.tvMemberSubtitle)
                    val ivAvatar = v.findViewById<ImageView>(R.id.ivMemberAvatar)
                    val displayName = if (item.userId == -1) {
                        "Add members"
                    } else {
                        // Always show username in members dialog, not nickname
                        // If the member is the current user and the stored username is a placeholder like "User {id}", use the session username.
                        val sessName = sessionManager.getUsername() ?: "Me"
                        val uname = item.username
                        if (item.userId == sessionManager.getUserId() && (uname == null || uname.startsWith("User "))) {
                            sessName
                        } else {
                            uname ?: "User ${item.userId}"
                        }
                    }
                    tvName.text = displayName
                    // Show Admin label under username when applicable
                    tvSubtitle.text = if (item.isAdmin) "Admin" else ""
                    ivAvatar.setImageResource(if (item.userId == -1) R.drawable.ic_add_person else R.drawable.ic_user_placeholder)
                    v.setOnClickListener {
                        if (item.userId == -1) {
                            openAddMembers()
                        } else {
                            // Short click: maybe future profile view
                        }
                    }

                    v.setOnLongClickListener {
                        if (item.userId == -1) {
                            openAddMembers(); return@setOnLongClickListener true
                        }

                        // Build actions depending on viewer permissions
                        val actions = mutableListOf<String>()
                        actions.add("Message")
                        if (viewerIsAdmin && item.userId != sessionManager.getUserId()) {
                            if (!item.isAdmin) actions.add("Make admin")
                            actions.add("Remove from group")
                        }

                        if (actions.isEmpty()) return@setOnLongClickListener true

                        AlertDialog.Builder(this@GroupChatActivity)
                            .setTitle(item.username ?: "Member")
                            .setItems(actions.toTypedArray()) { _, which ->
                                val choice = actions[which]
                                when (choice) {
                                    "Message" -> {
                                        // open direct chat or show toast for now
                                        Toast.makeText(this@GroupChatActivity, "Message ${item.username}", Toast.LENGTH_SHORT).show()
                                    }
                                    "Make admin" -> {
                                        lifecycleScope.launch(Dispatchers.IO) {
                                            try {
                                                val ok = networkManager.makeAdmin(groupId, item.userId)
                                                launch(Dispatchers.Main) {
                                                    if (ok) Toast.makeText(this@GroupChatActivity, "Promoted to admin", Toast.LENGTH_SHORT).show()
                                                    else Toast.makeText(this@GroupChatActivity, "Failed to promote", Toast.LENGTH_SHORT).show()
                                                }
                                            } catch (e: Exception) {
                                                launch(Dispatchers.Main) {
                                                    Toast.makeText(this@GroupChatActivity, "Network error", Toast.LENGTH_SHORT).show()
                                                }
                                            }
                                        }
                                    }
                                    "Remove from group" -> {
                                        AlertDialog.Builder(this@GroupChatActivity)
                                            .setTitle("Remove member")
                                            .setMessage("Remove ${item.username} from the group?")
                                            .setPositiveButton("Remove") { _, _ ->
                                                lifecycleScope.launch(Dispatchers.IO) {
                                                    try {
                                                        val ok = networkManager.removeFromGroup(groupId, item.userId)
                                                        launch(Dispatchers.Main) {
                                                            if (ok) Toast.makeText(this@GroupChatActivity, "Member removed", Toast.LENGTH_SHORT).show()
                                                            else Toast.makeText(this@GroupChatActivity, "Failed to remove", Toast.LENGTH_SHORT).show()
                                                        }
                                                    } catch (e: Exception) {
                                                        launch(Dispatchers.Main) {
                                                            Toast.makeText(this@GroupChatActivity, "Network error", Toast.LENGTH_SHORT).show()
                                                        }
                                                    }
                                                }
                                            }
                                            .setNegativeButton("Cancel", null)
                                            .show()
                                    }
                                }
                            }
                            .show()

                        true
                    }
                }

                override fun getItemCount(): Int = list.size
            }
            rv.adapter = adapter

            // Resolve placeholder usernames asynchronously and update adapter items when found
            lifecycleScope.launch(Dispatchers.IO) {
                val toResolve = list.filter { it.userId != -1 && (it.username.startsWith("User ") || it.username.isBlank()) }
                for (member in toResolve) {
                    try {
                        val resolved = db.friendDao().getFriend(member.userId)?.username
                            ?: db.userDao().getUser(member.userId)?.username
                        if (!resolved.isNullOrEmpty() && resolved != member.username) {
                            val idx = list.indexOfFirst { it.userId == member.userId }
                            if (idx >= 0) {
                                // update a copy of the GroupMember (data class) with the resolved username
                                val updated = list[idx].copy(username = resolved)
                                list[idx] = updated
                                lifecycleScope.launch(Dispatchers.Main) {
                                    adapter.notifyItemChanged(idx)
                                }
                            }
                        }
                    } catch (e: Exception) {
                        // ignore resolution errors for individual members
                    }
                }
            }
        }
        live.observe(this, observer)

        AlertDialog.Builder(this)
            .setView(dialogView)
            .setPositiveButton("OK", null)
            .show()
    }

    private fun showSetNicknameDialog() {
        // Inflate the richer nickname dialog and populate with group members
        val dialogView = layoutInflater.inflate(R.layout.dialog_nicknames, null)
        val rv = dialogView.findViewById<androidx.recyclerview.widget.RecyclerView>(R.id.rvNicknames)
        rv.layoutManager = androidx.recyclerview.widget.LinearLayoutManager(this)

        val db = ChatDatabase.getDatabase(this)
        val live = db.groupMemberDao().getGroupMembers(groupId)
        lateinit var observer: androidx.lifecycle.Observer<List<GroupMember>>
        observer = androidx.lifecycle.Observer { members ->
            val list = (members ?: emptyList()).toMutableList()
            val adapter = com.example.myapplication.ui.adapter.MemberNicknameAdapter(list) { gm ->
                // Edit callback: allow editing nickname for any member
                val input = EditText(this)
                input.setText(gm.nickname ?: gm.username)
                AlertDialog.Builder(this)
                    .setTitle("Nickname for ${gm.username}")
                    .setView(input)
                    .setPositiveButton("OK") { _, _ ->
                        val newNick = input.text.toString().trim()
                        if (newNick.isNotEmpty()) {
                            lifecycleScope.launch(Dispatchers.IO) {
                                try {
                                    // update local DB (upsert by composite PK)
                                    val updated = GroupMember(groupId = groupId, userId = gm.userId, username = gm.username, nickname = newNick, isAdmin = gm.isAdmin, joinedAt = gm.joinedAt)
                                    db.groupMemberDao().insertMember(updated)
                                    // notify server (best-effort)
                                    try {
                                        if (networkManager.isConnected()) {
                                            networkManager.setGroupNickname(groupId, gm.userId, newNick)
                                        }
                                    } catch (e: Exception) {
                                        // ignore network errors
                                    }
                                    launch(Dispatchers.Main) {
                                        Toast.makeText(this@GroupChatActivity, "Nickname updated", Toast.LENGTH_SHORT).show()
                                    }
                                } catch (e: Exception) {
                                    // ignore
                                }
                            }
                        }
                    }
                    .setNegativeButton("Cancel", null)
                    .show()
            }
            rv.adapter = adapter
            // Asynchronously resolve placeholder usernames (e.g. "User 11") from friends/users table
            lifecycleScope.launch(Dispatchers.IO) {
                val toResolve = list.filter { it.userId != -1 && (it.username.startsWith("User ") || it.username.isBlank()) }
                for (member in toResolve) {
                    try {
                        val resolved = db.friendDao().getFriend(member.userId)?.username
                            ?: db.userDao().getUser(member.userId)?.username
                        if (!resolved.isNullOrEmpty() && resolved != member.username) {
                            val idx = list.indexOfFirst { it.userId == member.userId }
                            if (idx >= 0) {
                                val updated = list[idx].copy(username = resolved)
                                list[idx] = updated
                                lifecycleScope.launch(Dispatchers.Main) {
                                    adapter.notifyItemChanged(idx)
                                }
                            }
                        }
                    } catch (e: Exception) {
                        // ignore individual resolution errors
                    }
                }
            }
        }
        live.observe(this, observer)

        AlertDialog.Builder(this)
            .setTitle("Nicknames")
            .setView(dialogView)
            .setPositiveButton("Close", null)
            .show()
    }

    private fun openAddMembers() {
        try {
            val intent = Intent(this, com.example.myapplication.ui.activity.AddFriendActivity::class.java)
            intent.putExtra("groupId", groupId)
            startActivity(intent)
        } catch (e: Exception) {
            Toast.makeText(this, "Unable to open add members screen", Toast.LENGTH_SHORT).show()
        }
    }

    private fun confirmAndLeaveGroup() {
        AlertDialog.Builder(this)
            .setTitle("Leave group")
            .setMessage("Are you sure you want to leave this group?")
            .setPositiveButton("Leave") { _, _ ->
                // Optimistically remove local group and inform server
                try {
                    val db = ChatDatabase.getDatabase(this)
                    lifecycleScope.launch(Dispatchers.IO) {
                        try {
                            db.groupMemberDao().removeAllMembers(groupId)
                            db.groupDao().deleteGroup(groupId)
                        } catch (e: Exception) {
                            // ignore
                        }
                    }
                } catch (e: Exception) {
                    // ignore
                }

                if (networkManager.isConnected()) {
                    val ok = networkManager.leaveGroup(groupId)
                    if (!ok) {
                        Toast.makeText(this, "Leave request failed (not connected)", Toast.LENGTH_SHORT).show()
                    }
                } else {
                    Toast.makeText(this, "Offline: left locally, will sync when online", Toast.LENGTH_SHORT).show()
                }
                finish()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }
}
