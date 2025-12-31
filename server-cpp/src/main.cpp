#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <iostream>
#include <thread>
#include <memory>

#include "persistence.h"
#include "persistence_pg.h"
#include "server_state.h"
#include "message_handler.h"

int main() {
  // Initialize PostgreSQL persistence
  std::string conn_str = "host=localhost port=5432 dbname=chat_app "
                        "user=chat_app_user password=chat_app_password";

  auto pg_persistence = std::make_shared<persistence::PostgresPersistence>(conn_str);

  if (!pg_persistence->initialize()) {
    std::cerr << "[C++ SERVER] Failed to connect to PostgreSQL database" << std::endl;
    std::cerr << "[C++ SERVER] Falling back to file-based persistence" << std::endl;
    
    // Fallback to old file-based persistence
    int next_id = 1;
    persistence::load_users(
      server_state::g_users_by_name,
      server_state::g_users_by_id,
      server_state::g_friends,
      server_state::g_friend_requests,
      next_id
    );
    server_state::g_next_user_id = next_id;
  } else {
    std::cout << "[C++ SERVER] Connected to PostgreSQL successfully" << std::endl;
    // Load all users from database into memory cache
    std::vector<data::User> all_users = pg_persistence->get_all_users();
    int max_id = 0;
    
    for (const auto& user : all_users) {
      data::User suser;
      suser.id = user.id;
      suser.username = user.username;
      suser.password_hash = user.password_hash;
      suser.phone = user.phone;
      suser.socket_fd = -1;
      
      server_state::g_users_by_id[user.id] = suser;
      server_state::g_users_by_name[user.username] = suser;
      
      if (user.id > max_id) {
        max_id = user.id;
      }
      
      // Load friends
      std::vector<data::User> friends = pg_persistence->get_friends(user.id);
      for (const auto& f : friends) {
        server_state::g_friends[user.id][f.id] = true;
      }
      
      std::cout << "[C++ SERVER] Loaded user: " << user.username 
                << " (id=" << user.id << ", " << friends.size() << " friends)" << std::endl;
    }
    
    server_state::g_next_user_id = max_id + 1;
    server_state::g_pg_persistence = pg_persistence;
    
    std::cout << "[C++ SERVER] Loaded " << all_users.size() << " users from database" << std::endl;
    // Load groups and members into in-memory cache so group operations work immediately
    for (const auto& user : all_users) {
      auto groups = pg_persistence->get_user_groups(user.id);
      for (const auto& g : groups) {
        std::lock_guard<std::mutex> lock(server_state::g_mutex);
        if (!server_state::g_groups.count(g.id)) {
          data::Group dg;
          dg.id = g.id;
          dg.name = g.name;
          dg.ownerId = g.ownerId;
          // populate members
          auto members = pg_persistence->get_group_members(g.id);
          for (const auto& m : members) {
            dg.members[m.id] = true;
            // Load admin status for each member
            bool is_admin = pg_persistence->is_group_admin(g.id, m.id);
            dg.admins[m.id] = is_admin;
          }
          server_state::g_groups[g.id] = std::move(dg);
          if (g.id >= server_state::g_next_group_id) server_state::g_next_group_id = g.id + 1;
        }
      }
    }
  }
  
  // Create server socket
  int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    perror("socket");
    return 1;
  }
  
  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(8080);
  
  if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("bind");
    return 1;
  }
  
  if (listen(server_fd, 16) < 0) {
    perror("listen");
    return 1;
  }

  std::cout << "[C++ SERVER] Listening on 0.0.0.0:8080" << std::endl;

  // Accept connections
  while (true) {
    sockaddr_in caddr{};
    socklen_t clen = sizeof(caddr);
    int cfd = accept(server_fd, (sockaddr*)&caddr, &clen);
    
    if (cfd < 0) {
      perror("accept");
      continue;
    }
    
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &caddr.sin_addr, client_ip, INET_ADDRSTRLEN);
    std::cout << "[C++ SERVER] Client connected from " << client_ip 
              << ":" << ntohs(caddr.sin_port) << std::endl;
    
    std::thread(handler::handle_client, cfd, caddr).detach();
  }
  
  return 0;
}
