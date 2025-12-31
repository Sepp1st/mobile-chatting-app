#ifndef PERSISTENCE_PG_H
#define PERSISTENCE_PG_H

#include <string>
#include <vector>
#include <memory>
#include <libpq-fe.h>
#include "protocol.h"
#include "data_models.h"

namespace persistence {

class PostgresConnection {
private:
  PGconn* conn;
  std::string connection_string;
  
public:
  PostgresConnection(const std::string& conn_str);
  ~PostgresConnection();
  
  bool connect();
  void disconnect();
  bool is_connected() const;
  PGconn* get_connection() { return conn; }
  
  // Execute query and return result
  PGresult* execute(const std::string& query);
  PGresult* execute_params(const std::string& query, 
                          int n_params, 
                          const char* const* param_values);
};

class PostgresPersistence {
private:
  std::unique_ptr<PostgresConnection> db;
  
  // Helper methods
  std::string escape_string(const std::string& str);
  long long get_current_timestamp();
  
public:
  PostgresPersistence(const std::string& connection_string);
  ~PostgresPersistence();
  
  bool initialize();
  
  // ====================================================================
  // USER OPERATIONS
  // ====================================================================
  bool create_user(const std::string& username, 
                  const std::string& password_hash,
                  const std::string& phone,
                  int& out_user_id);
  
  bool get_user_by_username(const std::string& username, data::User& user);
  bool get_user_by_id(int user_id, data::User& user);
  bool update_user_online_status(int user_id, bool is_online);
  std::vector<data::User> get_all_users();
  
  // ====================================================================
  // FRIEND OPERATIONS
  // ====================================================================
  bool add_friend(int user_id_1, int user_id_2);
  bool remove_friend(int user_id_1, int user_id_2);
  bool are_friends(int user_id_1, int user_id_2);
  std::vector<data::User> get_friends(int user_id);
  
  // ====================================================================
  // FRIEND REQUEST OPERATIONS
  // ====================================================================
  bool send_friend_request(int from_user_id, int to_user_id);
  bool accept_friend_request(int from_user_id, int to_user_id);
  bool decline_friend_request(int from_user_id, int to_user_id);
  std::vector<data::FriendRequest> get_pending_requests(int user_id);
  bool has_pending_request(int from_user_id, int to_user_id);
  
  // ====================================================================
  // MESSAGE OPERATIONS
  // ====================================================================
  bool save_message(int from_user_id, int to_user_id, 
                   const std::string& content, long long timestamp);
  std::vector<data::Message> get_conversation(int user_id_1, int user_id_2, 
                                              int limit = 50);
  bool mark_messages_read(int to_user_id, int from_user_id);
  int get_unread_count(int user_id);
  bool delete_conversation(int user_id_1, int user_id_2);
  
  // ====================================================================
  // GROUP OPERATIONS
  // ====================================================================
  bool create_group(const std::string& name, int creator_id, int& out_group_id);
  bool add_group_member(int group_id, int user_id);
  bool remove_group_member(int group_id, int user_id);
  // Nickname functionality removed
  std::vector<data::Group> get_user_groups(int user_id);
  std::vector<data::User> get_group_members(int group_id);
  bool is_group_member(int group_id, int user_id);
  // Group invite operations
  bool create_group_invite(int group_id, int from_user_id, int to_user_id);
  std::vector<data::GroupInvite> get_pending_group_invites(int user_id);
  bool delete_group_invite(int group_id, int to_user_id);
  // Additional group operations
  bool rename_group(int group_id, const std::string& new_name);
  
  // ====================================================================
  // GROUP MESSAGE OPERATIONS
  // ====================================================================
  bool save_group_message(int group_id, int user_id, 
                         const std::string& content, long long timestamp);
  std::vector<data::GroupMessage> get_group_messages(int group_id, int limit = 50);
  
    // Admin helpers
    bool set_group_admin(int group_id, int user_id, bool is_admin);
    bool is_group_admin(int group_id, int user_id);
  
  // ====================================================================
  // ACTIVITY LOG OPERATIONS
  // ====================================================================
  bool log_activity(const std::string& log_type, int user_id, 
                   int target_user_id, const std::string& details);
  
  // ====================================================================
  // OFFLINE MESSAGE OPERATIONS
  // ====================================================================
  bool queue_offline_message(int to_user_id, int message_type, 
                            const std::string& payload, int from_user_id);
  std::vector<std::pair<int, std::string>> get_offline_messages(int user_id);
  bool mark_offline_messages_delivered(int user_id);
};

} // namespace persistence

#endif // PERSISTENCE_PG_H
