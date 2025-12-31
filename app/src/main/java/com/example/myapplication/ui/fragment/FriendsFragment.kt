package com.example.myapplication.ui.fragment

import android.content.Intent
import android.os.Bundle
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.fragment.app.Fragment
import androidx.fragment.app.viewModels
import androidx.recyclerview.widget.LinearLayoutManager
import com.example.myapplication.databinding.FragmentFriendsBinding
import com.example.myapplication.ui.adapter.FriendsAdapter
import com.example.myapplication.ui.adapter.FriendRequestAdapter
import com.example.myapplication.ui.viewmodel.FriendViewModel
import com.example.myapplication.ui.activity.AddFriendActivity
import com.example.myapplication.ui.activity.ChatActivity
import com.example.myapplication.ui.activity.MainActivity
import com.example.myapplication.network.NetworkManager
import com.example.myapplication.data.repository.SessionManager

class FriendsFragment : Fragment(), NetworkManager.MessageCallback {
    
    private var _binding: FragmentFriendsBinding? = null
    private val binding get() = _binding!!
    
    private val viewModel: FriendViewModel by viewModels()
    private lateinit var adapter: FriendsAdapter
    private lateinit var pendingAdapter: FriendRequestAdapter
    private lateinit var networkManager: NetworkManager
    private lateinit var sessionManager: SessionManager
    
    companion object {
        private const val TAG = "FriendsFragment"
    }
    
    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentFriendsBinding.inflate(inflater, container, false)
        return binding.root
    }
    
    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        
        networkManager = NetworkManager.getInstance()
        sessionManager = SessionManager.getInstance(requireContext())
        networkManager.setCallback(this)
        
        setupRecyclerViews()
        observeViewModel()
        setupAddFriendButton()
        
        // Consume any pending messages from MainActivity
        (activity as? MainActivity)?.consumePendingMessages(this)
    }

    override fun onResume() {
        super.onResume()
        // Ensure this fragment receives callbacks while visible and refresh friends/pending
        networkManager.setCallback(this)
        networkManager.getFriendsList()
    }
    
    override fun onMessageReceived(messageType: Int, payload: String) {
        Log.d(TAG, "Received message: type=$messageType, payload=$payload")
        
        when (messageType) {
            NetworkManager.MessageType.FRIEND_REQUEST -> {
                // Parse: fromUserId=X&fromUsername=Y
                val data = payload.split("&").associate {
                    val parts = it.split("=")
                    if (parts.size == 2) parts[0] to parts[1] else "" to ""
                }
                
                val fromUserId = data["fromUserId"]?.toIntOrNull() ?: return
                val fromUsername = data["fromUsername"] ?: "Unknown"
                
                Log.d(TAG, "Friend request received from userId=$fromUserId, username=$fromUsername")
                
                // Add to pending requests in database
                viewModel.addPendingFriendRequest(fromUserId, fromUsername)
                
                // Show notification or update UI
                activity?.runOnUiThread {
                    android.widget.Toast.makeText(
                        requireContext(),
                        "Friend request from $fromUsername",
                        android.widget.Toast.LENGTH_SHORT
                    ).show()
                }
            }
            NetworkManager.MessageType.SUCCESS -> {
                // Only extract pending requests - MainActivity handles friend list
                parseFriendsListResponse(payload)
            }
        }
    }
    
    private fun parseFriendsListResponse(payload: String) {
        try {
            // Parse: count=X&id0=..&name0=..&pendingCount=Y&req_id0=Z&req_name0=W...
            val data = payload.split("&").associate {
                val parts = it.split("=", limit = 2)
                if (parts.size == 2) parts[0] to parts[1] else "" to ""
            }
            
            val count = data["count"]?.toIntOrNull() ?: 0
            val pendingCount = data["pendingCount"]?.toIntOrNull() ?: 0
            Log.d(TAG, "Friends list response: count=$count, pendingCount=$pendingCount")

            // Build friends list from response
            val friends = mutableListOf<com.example.myapplication.data.model.Friend>()
            for (i in 0 until count) {
                val fid = data["id$i"]?.toIntOrNull()
                val fname = data["name$i"]
                if (fid != null && !fname.isNullOrEmpty()) {
                    friends.add(
                        com.example.myapplication.data.model.Friend(
                            userId = fid,
                            username = fname,
                            isOnline = false,
                            lastSeen = System.currentTimeMillis()
                        )
                    )
                }
            }
            // Replace local friends list atomically
            viewModel.replaceFriends(friends)

            // Gom lại và thay thế pending trong một lượt để tránh race condition
            val list = mutableListOf<Pair<Int, String>>()
            for (i in 0 until pendingCount) {
                val reqUserId = data["req_id$i"]?.toIntOrNull()
                val reqUsername = data["req_name$i"] ?: "Unknown"
                if (reqUserId != null) list.add(reqUserId to reqUsername)
            }
            if (list.isNotEmpty()) viewModel.replacePendingRequestsForCurrentUser(list)
            else viewModel.clearPendingRequestsForCurrentUser()
            
            if (pendingCount > 0) {
                activity?.runOnUiThread {
                    android.widget.Toast.makeText(
                        requireContext(),
                        "You have $pendingCount friend request(s)",
                        android.widget.Toast.LENGTH_SHORT
                    ).show()
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error parsing friends list response", e)
        }
    }
    
    private fun setupRecyclerViews() {
        // Pending requests adapter
        pendingAdapter = FriendRequestAdapter(
            onAcceptClick = { request ->
                viewModel.acceptFriendRequest(request.fromUserId, request.fromUsername)
                // Immediately remove from Room to update UI
                viewModel.removePendingRequest(request.fromUserId)
            },
            onDeclineClick = { request ->
                viewModel.declineFriendRequest(request.fromUserId)
                // Immediately remove from Room to update UI
                viewModel.removePendingRequest(request.fromUserId)
            }
        )
        
        binding.recyclerViewPending.apply {
            adapter = pendingAdapter
            layoutManager = LinearLayoutManager(requireContext())
        }
        
        // Friends adapter
        adapter = FriendsAdapter(
            onItemClick = { friend ->
                // Navigate to chat with friend
                val intent = Intent(requireContext(), ChatActivity::class.java)
                intent.putExtra("friendId", friend.userId)
                intent.putExtra("friendName", friend.username)
                startActivity(intent)
            },
            onUnfriendClick = { friend ->
                viewModel.unfriend(friend.userId)
            }
        )
        
        binding.recyclerView.apply {
            this.adapter = this@FriendsFragment.adapter
            layoutManager = LinearLayoutManager(requireContext())
        }
    }
    
    private fun observeViewModel() {
        // Observe friends list
        viewModel.getAllFriends().observe(viewLifecycleOwner) { friends ->
            adapter.submitList(friends)
            updateEmptyState(friends.isEmpty())
        }
        
        // Observe pending requests
        val userId = sessionManager.getUserId()
        viewModel.getPendingRequests(userId).observe(viewLifecycleOwner) { requests ->
            pendingAdapter.submitList(requests)
            
            // Show/hide pending section
            if (requests.isNotEmpty()) {
                binding.tvPendingTitle.visibility = View.VISIBLE
                binding.recyclerViewPending.visibility = View.VISIBLE
            } else {
                binding.tvPendingTitle.visibility = View.GONE
                binding.recyclerViewPending.visibility = View.GONE
            }
        }
    }
    
    private fun updateEmptyState(isEmpty: Boolean) {
        binding.tvEmpty.visibility = if (isEmpty) View.VISIBLE else View.GONE
    }
    
    private fun setupAddFriendButton() {
        binding.btnAddFriend.setOnClickListener {
            // Open AddFriendActivity with list of all users
            val intent = Intent(requireContext(), AddFriendActivity::class.java)
            startActivity(intent)
        }
    }
    
    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }
}
