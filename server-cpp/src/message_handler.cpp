#include "message_handler.h"
#include "protocol.h"
#include "network_utils.h"
#include "server_state.h"
#include "utils.h"
#include "logger.h"
#include "persistence.h"
#include "persistence_pg.h"
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <ctime>

namespace handler {

void handle_client(int client_fd, sockaddr_in addr) {
  using namespace server_state;
  using namespace network;
  using namespace utils;
  using namespace logger;
  
  char header_buf[proto::HEADER_SIZE];
  uint32_t current_user_id = 0;
  
  std::cout << "[C++ SERVER] handle_client started for fd=" << client_fd << std::endl;

  while (true) {
    ssize_t r = recv_all(client_fd, header_buf, sizeof(header_buf));
    if (r <= 0) {
      std::cout << "[C++ SERVER] Connection closed or error, fd=" << client_fd << std::endl;
      break;
    }
    
    // Parse header
    uint32_t magic, length, sender_user_id, checksum;
    uint64_t ts;
    uint8_t version, type;
    
    std::memcpy(&magic, header_buf + 0, 4);
    version = header_buf[4];
    type = header_buf[5];
    std::memcpy(&length, header_buf + 6, 4);
    
    uint32_t ts_high, ts_low;
    std::memcpy(&ts_high, header_buf + 10, 4);
    std::memcpy(&ts_low, header_buf + 14, 4);
    ts = ((uint64_t)ntohl(ts_high) << 32) | ntohl(ts_low);
    
    std::memcpy(&sender_user_id, header_buf + 18, 4);
    std::memcpy(&checksum, header_buf + 22, 4);
    
    magic = ntohl(magic);
    length = ntohl(length);
    sender_user_id = ntohl(sender_user_id);
    checksum = ntohl(checksum);
    (void)ts; (void)checksum;

    // Use sender_user_id from packet header as the authoritative requester id
    // when available; otherwise fall back to the session's current_user_id.
    int requester_id = (sender_user_id != 0) ? (int)sender_user_id : (int)current_user_id;
    
    std::cout << "[C++ SERVER] Received: magic=0x" << std::hex << magic << std::dec 
              << " version=" << (int)version << " type=" << (int)type 
              << " length=" << length << std::endl;

    if (magic != proto::MAGIC || version != proto::VERSION) {
      std::cout << "[C++ SERVER] Invalid header, closing connection" << std::endl;
      send_message(client_fd, proto::ERROR, "Invalid header", 0);
      break;
    }

    std::string payload;
    if (length > 0) {
      payload.resize(length);
      ssize_t payload_recv = recv_all(client_fd, payload.data(), length);
      std::cout << "[C++ SERVER] Received payload: " << payload_recv << " bytes" << std::endl;
      if (payload_recv <= 0) {
        std::cout << "[C++ SERVER] Failed to receive payload" << std::endl;
        break;
      }
      std::cout << "[C++ SERVER] Payload: " << payload << std::endl;
    }

    // Handle different message types
    switch (type) {
      case proto::HEARTBEAT: {
        send_message(client_fd, proto::SUCCESS, "", current_user_id);
        break;
      }
      
      case proto::REGISTER: {
        auto kv = parse_kv(payload);
        std::string username = kv["username"];
        std::string password = kv["password"];
        std::string email = kv["email"];
        std::string phone = kv["phone"];
        
        if (username.empty() || password.empty()) {
          send_message(client_fd, proto::ERROR, "Missing username/password", 0);
          break;
        }
        
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_users_by_name.count(username)) {
          send_message(client_fd, proto::ERROR, "Username already exists", 0);
          break;
        }
        
        int uid = g_next_user_id++;
        data::User u;
        u.id = uid;
        u.username = username;
        u.password_hash = password;
        u.phone = phone;
        u.socket_fd = client_fd;
        u.is_online = true;
        u.last_seen = 0;
        
        g_users_by_name[username] = u;
        g_users_by_id[uid] = u;
        g_friends[uid];
        g_friend_requests[uid];
        current_user_id = uid;
        g_online_clients[uid] = client_fd;
        
        // Save to database if available
        if (g_pg_persistence) {
          int new_id;
          g_pg_persistence->create_user(username, password, phone, new_id);
          g_pg_persistence->update_user_online_status(uid, true);
        } else {
          persistence::save_users(g_users_by_name);
        }
        
        log_activity("REGISTER", uid, "username=" + username);
        
        auto resp = serialize_kv({
          {"success", "true"},
          {"userId", std::to_string(uid)},
          {"token", "demo_token"},
          {"username", username}
        });
        send_message(client_fd, proto::SUCCESS, resp, uid);
        break;
      }
      
      case proto::LOGIN: {
        auto kv = parse_kv(payload);
        std::string username = kv["username"];
        std::string password = kv["password"];
        int uid;
        std::string resp;
        std::vector<int> pendingRequests;
        std::vector<data::PendingMsg> offlineMessages;
        
        {
          std::lock_guard<std::mutex> lock(g_mutex);
          auto it = g_users_by_name.find(username);
          if (it == g_users_by_name.end() || it->second.password_hash != password) {
            send_message(client_fd, proto::ERROR, "Invalid credentials", 0);
            break;
          }
          
          uid = it->second.id;
          current_user_id = uid;
          g_online_clients[uid] = client_fd;
          
          // Update online status in database
          if (g_pg_persistence) {
            g_pg_persistence->update_user_online_status(uid, true);
          }
          
          resp = serialize_kv({
            {"success", "true"},
            {"userId", std::to_string(uid)},
            {"token", "demo_token"},
            {"username", username}
          });
          
          // Load pending friend requests from database
          if (g_pg_persistence) {
            auto db_requests = g_pg_persistence->get_pending_requests(uid);
            for (const auto& req : db_requests) {
              pendingRequests.push_back(req.from_user_id);
              // Also sync to memory
              auto& vec = g_friend_requests[uid];
              if (std::find(vec.begin(), vec.end(), req.from_user_id) == vec.end()) {
                vec.push_back(req.from_user_id);
              }
            }
          } else {
            // Fallback to memory-only
            auto it2 = g_friend_requests.find(uid);
            if (it2 != g_friend_requests.end() && !it2->second.empty()) {
              pendingRequests = it2->second;
            }
          }
          
          auto it3 = g_offline_messages.find(uid);
          if (it3 != g_offline_messages.end()) {
            offlineMessages = it3->second;
            g_offline_messages.erase(it3);
          }
        }
        
        send_message(client_fd, proto::SUCCESS, resp, uid);
        log_activity("LOGIN", uid, "username=" + username);
        notify_friends_status_change(uid, true);
        
        if (!pendingRequests.empty()) {
          std::cout << "[DEBUG] Delivering " << pendingRequests.size() 
                    << " pending friend requests to userId=" << uid << std::endl;
          for (int fromUserId : pendingRequests) {
            std::string fromUsername;
            {
              std::lock_guard<std::mutex> lock(g_mutex);
              fromUsername = g_users_by_id.count(fromUserId) 
                           ? g_users_by_id[fromUserId].username 
                           : ("User" + std::to_string(fromUserId));
            }
            auto notif = serialize_kv({
              {"fromUserId", std::to_string(fromUserId)},
              {"fromUsername", fromUsername}
            });
            send_message(client_fd, proto::FRIEND_REQUEST, notif, fromUserId);
          }
        }
        
        for (auto& pm : offlineMessages) {
          send_message(client_fd, pm.type, pm.payload, pm.fromUserId);
        }
        break;
      }
      
      case proto::LOGOUT: {
        uint32_t uid = current_user_id;
        {
          std::lock_guard<std::mutex> lock(g_mutex);
          if (uid) g_online_clients.erase(uid);
        }
        if (uid) {
          log_activity("LOGOUT", uid, "");
          notify_friends_status_change(uid, false);
        }
        send_message(client_fd, proto::SUCCESS, "", uid);
        goto cleanup;
      }
      
      case proto::SEARCH_USER: {
        auto kv = parse_kv(payload);
        std::string query = kv["query"];
        std::cout << "[SEARCH] Query: '" << query << "'" << std::endl;
        
        std::lock_guard<std::mutex> lock(g_mutex);
        std::vector<std::pair<std::string, std::string>> items;
        int count = 0;
        
        for (auto& pair : g_users_by_name) {
          const data::User& user = pair.second;
          if (user.id == requester_id) continue;
          
          std::string lowerUsername = user.username;
          std::string lowerQuery = query;
          std::transform(lowerUsername.begin(), lowerUsername.end(), 
                        lowerUsername.begin(), ::tolower);
          std::transform(lowerQuery.begin(), lowerQuery.end(), 
                        lowerQuery.begin(), ::tolower);
          
          if (lowerUsername.find(lowerQuery) != std::string::npos) {
            bool isFriend = g_friends[requester_id].count(user.id) && 
                           g_friends[requester_id][user.id];
            items.push_back({"id" + std::to_string(count), std::to_string(user.id)});
            items.push_back({"name" + std::to_string(count), user.username});
            items.push_back({"phone" + std::to_string(count), user.phone});
            items.push_back({"isFriend" + std::to_string(count), isFriend ? "1" : "0"});
            count++;
            if (count >= 20) break;
          }
        }
        
        items.insert(items.begin(), {"count", std::to_string(count)});
        std::cout << "[SEARCH] Found " << count << " users matching '" << query << "'" << std::endl;
        send_message(client_fd, proto::SUCCESS, serialize_kv(items), requester_id);
        break;
      }
      
      case proto::GET_ALL_USERS: {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::vector<std::pair<std::string, std::string>> items;
        int count = 0;
        
        for (auto& pair : g_users_by_id) {
          int uid = pair.first;
          if (uid == requester_id) continue;
          std::string uname = pair.second.username;
          bool isFriend = g_friends[requester_id].count(uid) && 
                         g_friends[requester_id][uid];
          items.push_back({"id" + std::to_string(count), std::to_string(uid)});
          items.push_back({"name" + std::to_string(count), uname});
          items.push_back({"isFriend" + std::to_string(count), isFriend ? "1" : "0"});
          count++;
        }
        
        items.insert(items.begin(), {"count", std::to_string(count)});
        send_message(client_fd, proto::SUCCESS, serialize_kv(items), requester_id);
        break;
      }
      
      case proto::GET_FRIENDS_LIST: {
        // Build friends list and pending requests (DB-backed + in-memory fallback)
        std::vector<std::pair<std::string, std::string>> items;
        int friendCount = 0;
        std::vector<int> pendingFromUsers;

        {
          std::lock_guard<std::mutex> lock(g_mutex);
          auto& fr = g_friends[current_user_id];
          
          std::cout << "[DEBUG] GET_FRIENDS_LIST: g_friends[" << current_user_id 
                    << "].size=" << fr.size() << std::endl;
          
          for (auto& p : fr) {
            if (p.second) {
              friendCount++;
              std::cout << "[DEBUG]   Friend: userId=" << p.first << std::endl;
            }
          }

          items.push_back({"count", std::to_string(friendCount)});

          int i = 0;
          for (auto& p : fr) {
            if (!p.second) continue;
            int fid = p.first;
            std::string uname = g_users_by_id.count(fid)
                              ? g_users_by_id[fid].username
                              : ("User" + std::to_string(fid));
            items.push_back({"id" + std::to_string(i), std::to_string(fid)});
            items.push_back({"name" + std::to_string(i), uname});
            ++i;
          }

          // Start with any in-memory pending requests
          auto itPend = g_friend_requests.find(current_user_id);
          if (itPend != g_friend_requests.end()) {
            pendingFromUsers = itPend->second;
          }
        }

        // Merge with DB pending requests to make it robust across restarts
        if (g_pg_persistence) {
          auto db_requests = g_pg_persistence->get_pending_requests((int)current_user_id);
          for (const auto& req : db_requests) {
            if (std::find(pendingFromUsers.begin(), pendingFromUsers.end(), req.from_user_id) == pendingFromUsers.end()) {
              pendingFromUsers.push_back(req.from_user_id);
            }
          }

          // Sync back to memory for future quick lookups
          {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto& memPend = g_friend_requests[current_user_id];
            for (int from : pendingFromUsers) {
              if (std::find(memPend.begin(), memPend.end(), from) == memPend.end()) {
                memPend.push_back(from);
              }
            }
          }
        }

        std::cout << "[DEBUG] GET_FRIENDS_LIST for userId=" << current_user_id
                  << ", pendingCount=" << pendingFromUsers.size() << std::endl;

        items.push_back({"pendingCount", std::to_string((int)pendingFromUsers.size())});
        for (int j = 0; j < (int)pendingFromUsers.size(); ++j) {
          int from = pendingFromUsers[j];
          std::string uname;
          {
            std::lock_guard<std::mutex> lock(g_mutex);
            uname = g_users_by_id.count(from)
                  ? g_users_by_id[from].username
                  : ("User" + std::to_string(from));
          }
          items.push_back({"req_id" + std::to_string(j), std::to_string(from)});
          items.push_back({"req_name" + std::to_string(j), uname});
        }

        send_message(client_fd, proto::SUCCESS, serialize_kv(items), current_user_id);
        break;
      }
      
      case proto::FRIEND_REQUEST: {
        auto kv = parse_kv(payload);
        int targetId = -1;
        
        if (kv.count("username")) {
          std::string targetUsername = kv["username"];
          std::lock_guard<std::mutex> lock(g_mutex);
          bool found = false;
          for (auto& pair : g_users_by_id) {
            if (pair.second.username == targetUsername) {
              targetId = pair.first;
              found = true;
              break;
            }
          }
          if (!found) {
            send_message(client_fd, proto::ERROR, "User not found", current_user_id);
            break;
          }
        } else if (kv.count("targetUserId")) {
          targetId = std::stoi(kv["targetUserId"]);
        } else {
          send_message(client_fd, proto::ERROR, "Missing username or targetUserId", 
                      current_user_id);
          break;
        }
        
        if (targetId == (int)current_user_id) {
          send_message(client_fd, proto::ERROR, "Cannot friend yourself", current_user_id);
          break;
        }
        
        {
          std::lock_guard<std::mutex> lock(g_mutex);
          bool alreadyFriends = (g_friends[current_user_id].count(targetId) && 
                                g_friends[current_user_id][targetId]) ||
                               (g_friends[targetId].count(current_user_id) && 
                                g_friends[targetId][current_user_id]);
          if (alreadyFriends) {
            send_message(client_fd, proto::ERROR, "Already friends", current_user_id);
            break;
          }
          
          // Save to PostgreSQL database
          if (g_pg_persistence) {
            // Check if already has pending request
            if (g_pg_persistence->has_pending_request(current_user_id, targetId)) {
              send_message(client_fd, proto::ERROR, "Friend request already sent", 
                          current_user_id);
              break;
            }
            
            // Save friend request to database
            if (!g_pg_persistence->send_friend_request(current_user_id, targetId)) {
              send_message(client_fd, proto::ERROR, "Failed to send friend request", 
                          current_user_id);
              break;
            }
          }
          
          // Also keep in memory for runtime
          auto& vec = g_friend_requests[targetId];
          if (std::find(vec.begin(), vec.end(), (int)current_user_id) == vec.end()) {
            vec.push_back(current_user_id);
            std::cout << "[DEBUG] FRIEND_REQUEST: userId=" << current_user_id 
                      << " -> targetId=" << targetId << ", stored successfully" << std::endl;
          } else {
            send_message(client_fd, proto::ERROR, "Friend request already sent", 
                        current_user_id);
            break;
          }
        }
        
        {
          std::lock_guard<std::mutex> lock(g_mutex);
          auto it = g_online_clients.find(targetId);
          if (it != g_online_clients.end()) {
            std::string fromUsername = g_users_by_id.count(current_user_id) 
                                      ? g_users_by_id[current_user_id].username 
                                      : ("User" + std::to_string(current_user_id));
            std::cout << "[DEBUG] Notifying targetUserId=" << targetId 
                      << " (fd=" << it->second << ") about friend request from userId=" 
                      << current_user_id << " (" << fromUsername << ")" << std::endl;
            auto notif = serialize_kv({
              {"fromUserId", std::to_string(current_user_id)},
              {"fromUsername", fromUsername}
            });
            send_message(it->second, proto::FRIEND_REQUEST, notif, current_user_id);
          } else {
            std::cout << "[DEBUG] targetUserId=" << targetId 
                      << " is NOT online, friend request will be delivered on next login" 
                      << std::endl;
          }
        }
        
        log_activity("FRIEND_REQUEST", current_user_id, 
                    "to_userId=" + std::to_string(targetId));
        send_message(client_fd, proto::SUCCESS, "Friend request sent", current_user_id);
        break;
      }
      
      case proto::FRIEND_ACCEPT: {
        auto kv = parse_kv(payload);
        int other = std::stoi(kv["userId"]);
        
        std::cout << "[DEBUG] FRIEND_ACCEPT: userId=" << current_user_id 
                  << " accepting request from userId=" << other << std::endl;
        
        std::lock_guard<std::mutex> lock(g_mutex);
        
        // Update database first
        if (g_pg_persistence) {
          bool success = g_pg_persistence->accept_friend_request(other, current_user_id);
          if (!success) {
            std::cout << "[ERROR] Failed to accept friend request in DB" << std::endl;
            send_message(client_fd, proto::ERROR, "Failed to accept friend request", current_user_id);
            break;
          }
        }
        
        // Update memory - remove from pending lists
        auto& vec = g_friend_requests[current_user_id];
        vec.erase(std::remove(vec.begin(), vec.end(), other), vec.end());
        
        // Also remove from the other user's sent list if exists
        auto& otherVec = g_friend_requests[other];
        otherVec.erase(std::remove(otherVec.begin(), otherVec.end(), current_user_id), otherVec.end());
        
        // Add to friends
        g_friends[current_user_id][other] = true;
        g_friends[other][current_user_id] = true;
        
        std::cout << "[DEBUG] FRIEND_ACCEPT: Success, now friends. " 
                  << "g_friends[" << current_user_id << "].size=" << g_friends[current_user_id].size()
                  << ", g_friends[" << other << "].size=" << g_friends[other].size() << std::endl;
        
        send_message(client_fd, proto::SUCCESS, "Friend request accepted", current_user_id);
        break;
      }
      
      case proto::FRIEND_DECLINE: {
        auto kv = parse_kv(payload);
        int other = std::stoi(kv["userId"]);
        std::lock_guard<std::mutex> lock(g_mutex);
        
        // Update database
        if (g_pg_persistence) {
          g_pg_persistence->decline_friend_request(other, current_user_id);
        }
        
        // Update memory
        auto& vec = g_friend_requests[current_user_id];
        vec.erase(std::remove(vec.begin(), vec.end(), other), vec.end());
        
        send_message(client_fd, proto::SUCCESS, "Friend request declined", current_user_id);
        break;
      }
      
      case proto::UNFRIEND: {
        auto kv = parse_kv(payload);
        int other = std::stoi(kv["userId"]);
        std::lock_guard<std::mutex> lock(g_mutex);
        
        g_friends[current_user_id].erase(other);
        g_friends[other].erase(current_user_id);
        
        auto& vec1 = g_friend_requests[current_user_id];
        vec1.erase(std::remove(vec1.begin(), vec1.end(), other), vec1.end());
        auto& vec2 = g_friend_requests[other];
        vec2.erase(std::remove(vec2.begin(), vec2.end(), current_user_id), vec2.end());
        
        send_message(client_fd, proto::SUCCESS, "Unfriended", current_user_id);
        break;
      }
      
      case proto::DIRECT_MESSAGE: {
        auto kv = parse_kv(payload);
        int recipientId = std::stoi(kv["recipientId"]);
        std::string message = kv["message"];
        
        auto out = serialize_kv({
          {"senderId", std::to_string((int)current_user_id)},
          {"message", message},
          {"timestamp", std::to_string((long long)time(nullptr))}
        });
        
        int target_fd = -1;
        {
          std::lock_guard<std::mutex> lock(g_mutex);
          auto it = g_online_clients.find(recipientId);
          if (it != g_online_clients.end()) target_fd = it->second;
        }
        
        if (target_fd != -1) {
          send_message(target_fd, proto::DIRECT_MESSAGE, out, current_user_id);
        } else {
          enqueue_offline(recipientId, proto::DIRECT_MESSAGE, out, current_user_id);
        }
        
        log_activity("DIRECT_MESSAGE", current_user_id, 
                    "to_userId=" + std::to_string(recipientId));
        send_message(client_fd, proto::SUCCESS, "Message sent", current_user_id);
        break;
      }
      
      case proto::CREATE_GROUP: {
        auto kv = parse_kv(payload);
        std::string name = kv.count("name") ? kv["name"] 
                         : ("group_" + std::to_string((long long)time(nullptr)));
        int gid;
        {
          std::lock_guard<std::mutex> lock(g_mutex);
          gid = g_next_group_id++;
          data::Group g{gid, name, (int)current_user_id, {}};
          g.members[(int)current_user_id] = true;
          g_groups[gid] = std::move(g);
        }
        
        log_activity("CREATE_GROUP", current_user_id, 
                    "groupId=" + std::to_string(gid) + " name=" + name);
        auto resp = serialize_kv({
          {"groupId", std::to_string(gid)},
          {"name", name}
        });
        send_message(client_fd, proto::SUCCESS, resp, current_user_id);
        break;
      }
      
      case proto::INVITE_TO_GROUP: {
        auto kv = parse_kv(payload);
        int gid = std::stoi(kv["groupId"]);
        int uid = std::stoi(kv["userId"]);
        bool ok = false;
        
        {
          std::lock_guard<std::mutex> lock(g_mutex);
          auto it = g_groups.find(gid);
          if (it != g_groups.end() && it->second.members.count((int)current_user_id)) {
            it->second.members[uid] = true;
            ok = true;
          }
        }
        
        if (ok) {
          int fd = -1;
          {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_online_clients.find(uid);
            if (it != g_online_clients.end()) fd = it->second;
          }
          if (fd != -1) {
            auto notif = serialize_kv({
              {"groupId", std::to_string(gid)},
              {"name", g_groups[gid].name}
            });
            send_message(fd, proto::INVITE_TO_GROUP, notif, current_user_id);
          }
          send_message(client_fd, proto::SUCCESS, "Invited", current_user_id);
        } else {
          send_message(client_fd, proto::ERROR, "Invite failed", current_user_id);
        }
        break;
      }
      
      case proto::REMOVE_FROM_GROUP: {
        auto kv = parse_kv(payload);
        int gid = std::stoi(kv["groupId"]);
        int uid = std::stoi(kv["userId"]);
        bool ok = false;
        
        {
          std::lock_guard<std::mutex> lock(g_mutex);
          auto it = g_groups.find(gid);
          if (it != g_groups.end() && it->second.members.count((int)current_user_id)) {
            it->second.members.erase(uid);
            ok = true;
          }
        }
        
        send_message(client_fd, ok ? proto::SUCCESS : proto::ERROR, 
                    ok ? "Removed" : "Remove failed", current_user_id);
        break;
      }
      
      case proto::LEAVE_GROUP: {
        auto kv = parse_kv(payload);
        int gid = std::stoi(kv["groupId"]);
        bool ok = false;
        bool deleted = false;
        
        {
          std::lock_guard<std::mutex> lock(g_mutex);
          auto it = g_groups.find(gid);
          if (it != g_groups.end()) {
            it->second.members.erase((int)current_user_id);
            ok = true;
            if (it->second.members.empty()) {
              g_groups.erase(it);
              deleted = true;
            }
          }
        }
        
        send_message(client_fd, proto::SUCCESS, 
                    deleted ? "Group deleted" : "Left group", current_user_id);
        break;
      }
      
      case proto::GROUP_MESSAGE: {
        auto kv = parse_kv(payload);
        int gid = std::stoi(kv["groupId"]);
        std::string message = kv["message"];
        std::vector<int> members;
        std::string out;
        
        {
          std::lock_guard<std::mutex> lock(g_mutex);
          auto it = g_groups.find(gid);
          if (it == g_groups.end() || !it->second.members.count((int)current_user_id)) {
            send_message(client_fd, proto::ERROR, "Not in group", current_user_id);
            break;
          }
          for (auto& m : it->second.members) {
            if (m.first != (int)current_user_id) members.push_back(m.first);
          }
        }
        
        out = serialize_kv({
          {"groupId", std::to_string(gid)},
          {"senderId", std::to_string((int)current_user_id)},
          {"message", message},
          {"timestamp", std::to_string((long long)time(nullptr))}
        });
        
        for (int uid : members) {
          int fd = -1;
          {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_online_clients.find(uid);
            if (it != g_online_clients.end()) fd = it->second;
          }
          if (fd != -1) {
            send_message(fd, proto::GROUP_MESSAGE, out, current_user_id);
          } else {
            enqueue_offline(uid, proto::GROUP_MESSAGE, out, current_user_id);
          }
        }
        
        log_activity("GROUP_MESSAGE", current_user_id, 
                    "groupId=" + std::to_string(gid));
        send_message(client_fd, proto::SUCCESS, "Group message sent", current_user_id);
        break;
      }
      
      default: {
        send_message(client_fd, proto::SUCCESS, "", current_user_id);
        break;
      }
    }
  }

cleanup:
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (current_user_id) g_online_clients.erase(current_user_id);
  }
  if (current_user_id) notify_friends_status_change(current_user_id, false);
  ::close(client_fd);
}

} // namespace handler
