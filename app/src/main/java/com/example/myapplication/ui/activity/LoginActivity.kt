package com.example.myapplication.ui.activity

import android.content.Intent
import android.os.Bundle
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import androidx.activity.viewModels
import com.example.myapplication.databinding.ActivityLoginBinding
import com.example.myapplication.data.repository.SessionManager
import com.example.myapplication.network.NetworkManager
import com.example.myapplication.utils.ActivityLogger
import com.example.myapplication.service.ServerService
import com.example.myapplication.ui.viewmodel.FriendViewModel
import kotlinx.coroutines.launch

class LoginActivity : AppCompatActivity(), NetworkManager.MessageCallback {
    
    private lateinit var binding: ActivityLoginBinding
    private lateinit var networkManager: NetworkManager
    private lateinit var sessionManager: SessionManager
    private lateinit var logger: ActivityLogger
    private var isLoginMode = true
    private val friendViewModel: FriendViewModel by viewModels()
    
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        
        binding = ActivityLoginBinding.inflate(layoutInflater)
        setContentView(binding.root)
        
        // Start server service automatically
        ServerService.start(this)
        
        networkManager = NetworkManager.getInstance()
        sessionManager = SessionManager.getInstance(this)
        logger = ActivityLogger.getInstance(this)
        
        networkManager.setCallback(this)
        
        // Check if already logged in
        if (sessionManager.isLoggedIn()) {
            navigateToMain()
            return
        }
        
        setupUI()
    }
    
    private fun setupUI() {
        binding.btnLogin.setOnClickListener {
            if (isLoginMode) {
                performLogin()
            } else {
                performRegister()
            }
        }
        
        binding.tvSwitchMode.setOnClickListener {
            switchMode()
        }
        
        binding.btnConnect.setOnClickListener {
            connectToServer()
        }
    }
    
    private fun switchMode() {
        isLoginMode = !isLoginMode
        if (isLoginMode) {
            binding.btnLogin.text = "Login"
            binding.tvSwitchMode.text = "Don't have an account? Register"
            binding.layoutPhone.visibility = android.view.View.GONE
        } else {
            binding.btnLogin.text = "Register"
            binding.tvSwitchMode.text = "Already have an account? Login"
            binding.layoutPhone.visibility = android.view.View.VISIBLE
        }
    }
    
    private fun connectToServer() {
        val host = binding.etServerHost.text.toString().ifEmpty { "10.0.2.2"}
        val port = binding.etServerPort.text.toString().toIntOrNull() ?: 8080
        
        binding.btnConnect.isEnabled = false
        binding.progressBar.visibility = android.view.View.VISIBLE
        
        lifecycleScope.launch {
            logger.logConnection("CONNECT", host, port, false)
            
            val connected = networkManager.connect(host, port)
            
            if (connected) {
                logger.logConnection("CONNECT", host, port, true)
                sessionManager.saveServerConfig(host, port)
                Toast.makeText(this@LoginActivity, "Connected to server", Toast.LENGTH_SHORT).show()
                binding.layoutAuth.visibility = android.view.View.VISIBLE
            } else {
                logger.logConnection("CONNECT", host, port, false)
                Toast.makeText(this@LoginActivity, "Failed to connect to server", Toast.LENGTH_SHORT).show()
            }
            
            binding.btnConnect.isEnabled = true
            binding.progressBar.visibility = android.view.View.GONE
        }
    }
    
    private fun performLogin() {
        val username = binding.etUsername.text.toString()
        val password = binding.etPassword.text.toString()
        
        if (username.isEmpty() || password.isEmpty()) {
            Toast.makeText(this, "Please fill all fields", Toast.LENGTH_SHORT).show()
            return
        }
        
        binding.btnLogin.isEnabled = false
        binding.progressBar.visibility = android.view.View.VISIBLE
        
        lifecycleScope.launch {
            logger.logAuth("LOGIN", username, false)
            val success = networkManager.login(username, password)
            
            if (!success) {
                logger.logAuth("LOGIN", username, false)
                Toast.makeText(this@LoginActivity, "Login failed", Toast.LENGTH_SHORT).show()
                binding.btnLogin.isEnabled = true
                binding.progressBar.visibility = android.view.View.GONE
            }
            // Success will be handled in onMessageReceived
        }
    }
    
    private fun performRegister() {
        val username = binding.etUsername.text.toString()
        val password = binding.etPassword.text.toString()
        val phone = binding.etPhone.text.toString()
        
        if (username.isEmpty() || password.isEmpty() || phone.isEmpty()) {
            Toast.makeText(this, "Please fill all fields", Toast.LENGTH_SHORT).show()
            return
        }
        
        binding.btnLogin.isEnabled = false
        binding.progressBar.visibility = android.view.View.VISIBLE
        
        lifecycleScope.launch {
            logger.logAuth("REGISTER", username, false)
            val success = networkManager.register(username, password, "", phone)
            
            if (!success) {
                logger.logAuth("REGISTER", username, false)
                Toast.makeText(this@LoginActivity, "Registration failed", Toast.LENGTH_SHORT).show()
                binding.btnLogin.isEnabled = true
                binding.progressBar.visibility = android.view.View.GONE
            }
            // Success will be handled in onMessageReceived
        }
    }
    
    override fun onMessageReceived(messageType: Int, payload: String) {
        runOnUiThread {
            when (messageType) {
                NetworkManager.MessageType.SUCCESS -> {
                    // If this SUCCESS contains auth data, handle login/register success
                    if (payload.contains("userId=") && payload.contains("token=")) {
                        handleAuthSuccess(payload)
                    } else if (payload.contains("pendingCount=")) {
                        // Handle friends list payload (pending requests) during login
                        handleFriendsListSuccess(payload)
                    }
                }
                NetworkManager.MessageType.ERROR -> {
                    handleAuthError(payload)
                }
                NetworkManager.MessageType.FRIEND_REQUEST -> {
                    // Cache pending friend request during login so it shows after navigating
                    try {
                        val data = payload.split("&").associate {
                            val parts = it.split("=", limit = 2)
                            if (parts.size == 2) parts[0] to parts[1] else "" to ""
                        }
                        val fromUserId = data["fromUserId"]?.toIntOrNull()
                        val fromUsername = data["fromUsername"] ?: "Unknown"
                        if (fromUserId != null) {
                            friendViewModel.addPendingFriendRequest(fromUserId, fromUsername)
                        }
                    } catch (_: Exception) { }
                }
            }
        }
    }
    
    private fun handleAuthSuccess(payload: String) {
        lifecycleScope.launch {
            try {
                // Parse payload: "userId=123&token=abc&username=user"
                val data = payload.split("&").associate {
                    val (key, value) = it.split("=")
                    key to value
                }
                
                val userId = data["userId"]?.toIntOrNull() ?: return@launch
                val token = data["token"] ?: ""
                val username = data["username"] ?: binding.etUsername.text.toString()
                
                logger.logAuth(if (isLoginMode) "LOGIN" else "REGISTER", username, true)
                
                sessionManager.saveSession(userId, username, token)
                networkManager.setUserId(userId)

                // Ngay sau khi login thành công, gọi lấy danh sách bạn bè/pending
                // để bắt kịp các FRIEND_REQUEST đến sớm trong quá trình login
                networkManager.getFriendsList()
                
                Toast.makeText(this@LoginActivity, "Success!", Toast.LENGTH_SHORT).show()
                navigateToMain()
            } catch (e: Exception) {
                logger.logError("AUTH", "Error parsing auth response", e)
                Toast.makeText(this@LoginActivity, "Error: ${e.message}", Toast.LENGTH_SHORT).show()
                binding.btnLogin.isEnabled = true
                binding.progressBar.visibility = android.view.View.GONE
            }
        }
    }

    private fun handleFriendsListSuccess(payload: String) {
        try {
            val data = payload.split("&").associate {
                val parts = it.split("=", limit = 2)
                if (parts.size == 2) parts[0] to parts[1] else "" to ""
            }
            val pendingCount = data["pendingCount"]?.toIntOrNull() ?: 0

            val list = mutableListOf<Pair<Int, String>>()
            for (i in 0 until pendingCount) {
                val reqUserId = data["req_id$i"]?.toIntOrNull()
                val reqUsername = data["req_name$i"] ?: "Unknown"
                if (reqUserId != null) list.add(reqUserId to reqUsername)
            }
            friendViewModel.replacePendingRequestsForCurrentUser(list)
        } catch (_: Exception) { }
    }
    
    private fun handleAuthError(payload: String) {
        lifecycleScope.launch {
            logger.log(ActivityLogger.LogLevel.ERROR, "AUTH", "Auth error: $payload")
        }
        Toast.makeText(this, "Error: $payload", Toast.LENGTH_SHORT).show()
        binding.btnLogin.isEnabled = true
        binding.progressBar.visibility = android.view.View.GONE
    }
    
    private fun navigateToMain() {
        val intent = Intent(this, MainActivity::class.java)
        startActivity(intent)
        finish()
    }
    
    override fun onDestroy() {
        super.onDestroy()
        if (!sessionManager.isLoggedIn()) {
            networkManager.disconnect()
        }
    }
}
