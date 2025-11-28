#include "persistence_pg.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <chrono>
#include <algorithm>

namespace persistence {

// ====================================================================
// PostgresConnection Implementation
// ====================================================================

PostgresConnection::PostgresConnection(const std::string& conn_str)
  : conn(nullptr), connection_string(conn_str) {}

PostgresConnection::~PostgresConnection() {
  disconnect();
}

bool PostgresConnection::connect() {
  if (conn) {
    disconnect();
  }
  
  conn = PQconnectdb(connection_string.c_str());
  
  if (PQstatus(conn) != CONNECTION_OK) {
    std::cerr << "[PERSISTENCE] PostgreSQL connection failed: " 
              << PQerrorMessage(conn) << std::endl;
    PQfinish(conn);
    conn = nullptr;
    return false;
  }
  
  std::cout << "[PERSISTENCE] Connected to PostgreSQL successfully" << std::endl;
  return true;
}

void PostgresConnection::disconnect() {
  if (conn) {
    PQfinish(conn);
    conn = nullptr;
    std::cout << "[PERSISTENCE] Disconnected from PostgreSQL" << std::endl;
  }
}

bool PostgresConnection::is_connected() const {
  return conn != nullptr && PQstatus(conn) == CONNECTION_OK;
}

PGresult* PostgresConnection::execute(const std::string& query) {
  if (!is_connected()) {
    std::cerr << "[PERSISTENCE] Not connected to database" << std::endl;
    return nullptr;
  }
  
  PGresult* res = PQexec(conn, query.c_str());
  
  if (PQresultStatus(res) != PGRES_TUPLES_OK && 
      PQresultStatus(res) != PGRES_COMMAND_OK) {
    std::cerr << "[PERSISTENCE] Query failed: " << PQerrorMessage(conn) << std::endl;
    std::cerr << "[PERSISTENCE] Query: " << query << std::endl;
    PQclear(res);
    return nullptr;
  }
  
  return res;
}

PGresult* PostgresConnection::execute_params(const std::string& query,
                                             int n_params,
                                             const char* const* param_values) {
  if (!is_connected()) {
    std::cerr << "[PERSISTENCE] Not connected to database" << std::endl;
    return nullptr;
  }
  
  PGresult* res = PQexecParams(conn, query.c_str(), n_params, nullptr,
                               param_values, nullptr, nullptr, 0);
  
  if (PQresultStatus(res) != PGRES_TUPLES_OK && 
      PQresultStatus(res) != PGRES_COMMAND_OK) {
    std::cerr << "[PERSISTENCE] Parameterized query failed: " 
              << PQerrorMessage(conn) << std::endl;
    PQclear(res);
    return nullptr;
  }
  
  return res;
}

// ====================================================================
// PostgresPersistence Implementation
// ====================================================================

PostgresPersistence::PostgresPersistence(const std::string& connection_string) {
  db = std::make_unique<PostgresConnection>(connection_string);
}

PostgresPersistence::~PostgresPersistence() {}

bool PostgresPersistence::initialize() {
  return db->connect();
}

std::string PostgresPersistence::escape_string(const std::string& str) {
  char* escaped = new char[str.length() * 2 + 1];
  PQescapeStringConn(db->get_connection(), escaped, str.c_str(), str.length(), nullptr);
  std::string result(escaped);
  delete[] escaped;
  return result;
}

long long PostgresPersistence::get_current_timestamp() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()
  ).count();
}

// ====================================================================
// USER OPERATIONS
// ====================================================================

bool PostgresPersistence::create_user(const std::string& username,
                                     const std::string& password_hash,
                                     const std::string& phone,
                                     int& out_user_id) {
  std::string query = "INSERT INTO users (username, password_hash, phone, is_online) "
                     "VALUES ('" + escape_string(username) + "', '"
                     + escape_string(password_hash) + "', '"
                     + escape_string(phone) + "', FALSE) RETURNING id";
  
  PGresult* res = db->execute(query);
  if (!res) {
    return false;
  }
  
  out_user_id = std::stoi(PQgetvalue(res, 0, 0));
  PQclear(res);
  
  log_activity("USER_REGISTER", out_user_id, 0, "User registered: " + username);
  return true;
}

bool PostgresPersistence::get_user_by_username(const std::string& username, 
                                               data::User& user) {
  std::string query = "SELECT id, username, password_hash, phone, is_online, "
                     "last_seen "
                     "FROM users WHERE username = '" + escape_string(username) + "'";
  
  PGresult* res = db->execute(query);
  if (!res || PQntuples(res) == 0) {
    if (res) PQclear(res);
    return false;
  }
  
  user.id = std::stoi(PQgetvalue(res, 0, 0));
  user.username = PQgetvalue(res, 0, 1);
  user.password_hash = PQgetvalue(res, 0, 2);
  user.phone = PQgetvalue(res, 0, 3);
  user.is_online = (strcmp(PQgetvalue(res, 0, 4), "t") == 0);
  user.last_seen = PQgetisnull(res, 0, 5) ? 0 : std::stoll(PQgetvalue(res, 0, 5));
  
  PQclear(res);
  return true;
}

bool PostgresPersistence::get_user_by_id(int user_id, data::User& user) {
  std::string query = "SELECT id, username, password_hash, phone, is_online, "
                     "last_seen "
                     "FROM users WHERE id = " + std::to_string(user_id);
  
  PGresult* res = db->execute(query);
  if (!res || PQntuples(res) == 0) {
    if (res) PQclear(res);
    return false;
  }
  
  user.id = std::stoi(PQgetvalue(res, 0, 0));
  user.username = PQgetvalue(res, 0, 1);
  user.password_hash = PQgetvalue(res, 0, 2);
  user.phone = PQgetvalue(res, 0, 3);
  user.is_online = (strcmp(PQgetvalue(res, 0, 4), "t") == 0);
  user.last_seen = PQgetisnull(res, 0, 5) ? 0 : std::stoll(PQgetvalue(res, 0, 5));
  
  PQclear(res);
  return true;
}

bool PostgresPersistence::update_user_online_status(int user_id, bool is_online) {
  // Schema stores last_seen as BIGINT epoch milliseconds
  std::string query = "UPDATE users SET is_online = " 
                     + std::string(is_online ? "TRUE" : "FALSE")
                     + ", last_seen = (EXTRACT(EPOCH FROM NOW()) * 1000)::BIGINT WHERE id = "
                     + std::to_string(user_id);
  
  PGresult* res = db->execute(query);
  if (!res) {
    return false;
  }
  
  PQclear(res);
  log_activity(is_online ? "USER_ONLINE" : "USER_OFFLINE", user_id, 0, "");
  return true;
}

std::vector<data::User> PostgresPersistence::get_all_users() {
  std::vector<data::User> users;
  
  std::string query = "SELECT id, username, password_hash, phone, is_online, "
                     "last_seen "
                     "FROM users ORDER BY username";
  
  PGresult* res = db->execute(query);
  if (!res) {
    return users;
  }
  
  int rows = PQntuples(res);
  for (int i = 0; i < rows; i++) {
    data::User user;
    user.id = std::stoi(PQgetvalue(res, i, 0));
    user.username = PQgetvalue(res, i, 1);
    user.password_hash = PQgetvalue(res, i, 2);
    user.phone = PQgetvalue(res, i, 3);
    user.is_online = (strcmp(PQgetvalue(res, i, 4), "t") == 0);
    user.last_seen = PQgetisnull(res, i, 5) ? 0 : std::stoll(PQgetvalue(res, i, 5));
    users.push_back(user);
  }
  
  PQclear(res);
  return users;
}

// ====================================================================
// FRIEND OPERATIONS
// ====================================================================

bool PostgresPersistence::add_friend(int user_id_1, int user_id_2) {
  // Canonical ordering: user_id_1 < user_id_2
  if (user_id_1 > user_id_2) {
    std::swap(user_id_1, user_id_2);
  }
  
  std::string query = "INSERT INTO friends (user_id_1, user_id_2) VALUES ("
                     + std::to_string(user_id_1) + ", "
                     + std::to_string(user_id_2) + ") "
                     "ON CONFLICT DO NOTHING";
  
  PGresult* res = db->execute(query);
  if (!res) {
    return false;
  }
  
  PQclear(res);
  log_activity("FRIEND_ADD", user_id_1, user_id_2, "");
  return true;
}

bool PostgresPersistence::remove_friend(int user_id_1, int user_id_2) {
  if (user_id_1 > user_id_2) {
    std::swap(user_id_1, user_id_2);
  }
  
  std::string query = "DELETE FROM friends WHERE user_id_1 = "
                     + std::to_string(user_id_1) + " AND user_id_2 = "
                     + std::to_string(user_id_2);
  
  PGresult* res = db->execute(query);
  if (!res) {
    return false;
  }
  
  PQclear(res);
  log_activity("FRIEND_REMOVE", user_id_1, user_id_2, "");
  return true;
}

bool PostgresPersistence::are_friends(int user_id_1, int user_id_2) {
  if (user_id_1 > user_id_2) {
    std::swap(user_id_1, user_id_2);
  }
  
  std::string query = "SELECT 1 FROM friends WHERE user_id_1 = "
                     + std::to_string(user_id_1) + " AND user_id_2 = "
                     + std::to_string(user_id_2);
  
  PGresult* res = db->execute(query);
  if (!res) {
    return false;
  }
  
  bool result = PQntuples(res) > 0;
  PQclear(res);
  return result;
}

std::vector<data::User> PostgresPersistence::get_friends(int user_id) {
  std::vector<data::User> friends;
  
  std::string query = "SELECT u.id, u.username, u.password_hash, u.phone, "
                     "u.is_online, u.last_seen "
                     "FROM v_user_friends vuf JOIN users u ON vuf.friend_id = u.id "
                     "WHERE vuf.user_id = " + std::to_string(user_id)
                     + " ORDER BY u.username";
  
  PGresult* res = db->execute(query);
  if (!res) {
    return friends;
  }
  
  int rows = PQntuples(res);
  for (int i = 0; i < rows; i++) {
    data::User user;
    user.id = std::stoi(PQgetvalue(res, i, 0));
    user.username = PQgetvalue(res, i, 1);
    user.password_hash = PQgetvalue(res, i, 2);
    user.phone = PQgetvalue(res, i, 3);
    user.is_online = (strcmp(PQgetvalue(res, i, 4), "t") == 0);
    user.last_seen = PQgetisnull(res, i, 5) ? 0 : std::stoll(PQgetvalue(res, i, 5));
    friends.push_back(user);
  }
  
  PQclear(res);
  return friends;
}

// ====================================================================
// FRIEND REQUEST OPERATIONS
// ====================================================================

bool PostgresPersistence::send_friend_request(int from_user_id, int to_user_id) {
  // Ensure an upsert that re-opens previous requests by setting status back to PENDING
  // and refreshes updated_at, instead of doing nothing on conflict.
  std::string query =
      "INSERT INTO friend_requests (from_user_id, to_user_id, status) "
      "VALUES (" + std::to_string(from_user_id) + ", " + std::to_string(to_user_id) + ", 'PENDING') "
      "ON CONFLICT (from_user_id, to_user_id) DO UPDATE SET status = 'PENDING', updated_at = NOW()";

  PGresult* res = db->execute(query);
  if (!res) {
    return false;
  }

  PQclear(res);
  log_activity("FRIEND_REQUEST_SEND", from_user_id, to_user_id, "");
  return true;
}

bool PostgresPersistence::accept_friend_request(int from_user_id, int to_user_id) {
  // Begin transaction
  PGresult* res = db->execute("BEGIN");
  if (!res) return false;
  PQclear(res);
  
  // Add to friends table first
  if (!add_friend(from_user_id, to_user_id)) {
    db->execute("ROLLBACK");
    return false;
  }
  
  // Delete the friend request (instead of updating status)
  std::string delete_query = "DELETE FROM friend_requests WHERE from_user_id = "
                            + std::to_string(from_user_id) + " AND to_user_id = "
                            + std::to_string(to_user_id);
  res = db->execute(delete_query);
  if (!res) {
    db->execute("ROLLBACK");
    return false;
  }
  PQclear(res);
  
  // Commit transaction
  res = db->execute("COMMIT");
  if (!res) {
    db->execute("ROLLBACK");
    return false;
  }
  PQclear(res);
  
  log_activity("FRIEND_REQUEST_ACCEPT", to_user_id, from_user_id, "");
  return true;
}

bool PostgresPersistence::decline_friend_request(int from_user_id, int to_user_id) {
  // Delete the friend request instead of updating status
  std::string query = "DELETE FROM friend_requests WHERE from_user_id = "
                     + std::to_string(from_user_id) + " AND to_user_id = "
                     + std::to_string(to_user_id);
  
  PGresult* res = db->execute(query);
  if (!res) {
    return false;
  }
  
  PQclear(res);
  log_activity("FRIEND_REQUEST_DECLINE", to_user_id, from_user_id, "");
  return true;
}

std::vector<data::FriendRequest> PostgresPersistence::get_pending_requests(int user_id) {
  std::vector<data::FriendRequest> requests;
  
  std::string query = "SELECT id, from_user_id, to_user_id, status, "
                     "(EXTRACT(EPOCH FROM created_at) * 1000)::BIGINT "
                     "FROM friend_requests WHERE to_user_id = "
                     + std::to_string(user_id) + " AND status = 'PENDING' "
                     "ORDER BY created_at DESC";
  
  PGresult* res = db->execute(query);
  if (!res) {
    return requests;
  }
  
  int rows = PQntuples(res);
  for (int i = 0; i < rows; i++) {
    data::FriendRequest req;
    req.id = std::stoi(PQgetvalue(res, i, 0));
    req.from_user_id = std::stoi(PQgetvalue(res, i, 1));
    req.to_user_id = std::stoi(PQgetvalue(res, i, 2));
    req.status = PQgetvalue(res, i, 3);
    req.timestamp = std::stoll(PQgetvalue(res, i, 4));
    requests.push_back(req);
  }
  
  PQclear(res);
  return requests;
}

bool PostgresPersistence::has_pending_request(int from_user_id, int to_user_id) {
  std::string query = "SELECT 1 FROM friend_requests WHERE from_user_id = "
                     + std::to_string(from_user_id) + " AND to_user_id = "
                     + std::to_string(to_user_id) + " AND status = 'PENDING'";
  
  PGresult* res = db->execute(query);
  if (!res) {
    return false;
  }
  
  bool result = PQntuples(res) > 0;
  PQclear(res);
  return result;
}

// ====================================================================
// MESSAGE OPERATIONS
// ====================================================================

bool PostgresPersistence::save_message(int from_user_id, int to_user_id,
                                      const std::string& content,
                                      long long timestamp) {
  std::string query = "INSERT INTO messages (from_user_id, to_user_id, content, "
                     "timestamp, is_read) VALUES ("
                     + std::to_string(from_user_id) + ", "
                     + std::to_string(to_user_id) + ", '"
                     + escape_string(content) + "', to_timestamp("
                     + std::to_string(timestamp / 1000.0) + "), FALSE)";
  
  PGresult* res = db->execute(query);
  if (!res) {
    return false;
  }
  
  PQclear(res);
  return true;
}

std::vector<data::Message> PostgresPersistence::get_conversation(int user_id_1,
                                                                 int user_id_2,
                                                                 int limit) {
  std::vector<data::Message> messages;
  
  std::string query = "SELECT id, from_user_id, to_user_id, content, "
                     "(EXTRACT(EPOCH FROM timestamp) * 1000)::BIGINT, is_read "
                     "FROM messages WHERE "
                     "(from_user_id = " + std::to_string(user_id_1) +
                     " AND to_user_id = " + std::to_string(user_id_2) + ") OR "
                     "(from_user_id = " + std::to_string(user_id_2) +
                     " AND to_user_id = " + std::to_string(user_id_1) + ") "
                     "ORDER BY timestamp DESC LIMIT " + std::to_string(limit);
  
  PGresult* res = db->execute(query);
  if (!res) {
    return messages;
  }
  
  int rows = PQntuples(res);
  for (int i = 0; i < rows; i++) {
    data::Message msg;
    msg.id = std::stoi(PQgetvalue(res, i, 0));
    msg.from_user_id = std::stoi(PQgetvalue(res, i, 1));
    msg.to_user_id = std::stoi(PQgetvalue(res, i, 2));
    msg.content = PQgetvalue(res, i, 3);
    msg.timestamp = std::stoll(PQgetvalue(res, i, 4));
    msg.is_read = (strcmp(PQgetvalue(res, i, 5), "t") == 0);
    messages.push_back(msg);
  }
  
  PQclear(res);
  
  // Reverse to get chronological order
  std::reverse(messages.begin(), messages.end());
  return messages;
}

bool PostgresPersistence::mark_messages_read(int to_user_id, int from_user_id) {
  std::string query = "UPDATE messages SET is_read = TRUE WHERE to_user_id = "
                     + std::to_string(to_user_id) + " AND from_user_id = "
                     + std::to_string(from_user_id);
  
  PGresult* res = db->execute(query);
  if (!res) {
    return false;
  }
  
  PQclear(res);
  return true;
}

int PostgresPersistence::get_unread_count(int user_id) {
  std::string query = "SELECT COUNT(*) FROM messages WHERE to_user_id = "
                     + std::to_string(user_id) + " AND is_read = FALSE";
  
  PGresult* res = db->execute(query);
  if (!res || PQntuples(res) == 0) {
    if (res) PQclear(res);
    return 0;
  }
  
  int count = std::stoi(PQgetvalue(res, 0, 0));
  PQclear(res);
  return count;
}

// ====================================================================
// GROUP OPERATIONS
// ====================================================================

bool PostgresPersistence::create_group(const std::string& name, int creator_id,
                                      int& out_group_id) {
  // Begin transaction
  PGresult* res = db->execute("BEGIN");
  if (!res) return false;
  PQclear(res);
  
  // Create group
  std::string query = "INSERT INTO groups (name, creator_id) VALUES ('"
                     + escape_string(name) + "', " + std::to_string(creator_id)
                     + ") RETURNING id";
  
  res = db->execute(query);
  if (!res) {
    db->execute("ROLLBACK");
    return false;
  }
  
  out_group_id = std::stoi(PQgetvalue(res, 0, 0));
  PQclear(res);
  
  // Add creator as member
  if (!add_group_member(out_group_id, creator_id)) {
    db->execute("ROLLBACK");
    return false;
  }
  
  // Commit transaction
  res = db->execute("COMMIT");
  if (!res) {
    db->execute("ROLLBACK");
    return false;
  }
  PQclear(res);
  
  log_activity("GROUP_CREATE", creator_id, 0, "Group: " + name);
  return true;
}

bool PostgresPersistence::add_group_member(int group_id, int user_id) {
  std::string query = "INSERT INTO group_members (group_id, user_id) VALUES ("
                     + std::to_string(group_id) + ", "
                     + std::to_string(user_id) + ") ON CONFLICT DO NOTHING";
  
  PGresult* res = db->execute(query);
  if (!res) {
    return false;
  }
  
  PQclear(res);
  log_activity("GROUP_MEMBER_ADD", user_id, group_id, "");
  return true;
}

bool PostgresPersistence::remove_group_member(int group_id, int user_id) {
  std::string query = "DELETE FROM group_members WHERE group_id = "
                     + std::to_string(group_id) + " AND user_id = "
                     + std::to_string(user_id);
  
  PGresult* res = db->execute(query);
  if (!res) {
    return false;
  }
  
  PQclear(res);
  log_activity("GROUP_MEMBER_REMOVE", user_id, group_id, "");
  return true;
}

std::vector<data::Group> PostgresPersistence::get_user_groups(int user_id) {
  std::vector<data::Group> groups;
  
  std::string query = "SELECT g.id, g.name, g.creator_id FROM groups g "
                     "JOIN group_members gm ON g.id = gm.group_id "
                     "WHERE gm.user_id = " + std::to_string(user_id)
                     + " ORDER BY g.name";
  
  PGresult* res = db->execute(query);
  if (!res) {
    return groups;
  }
  
  int rows = PQntuples(res);
  for (int i = 0; i < rows; i++) {
    data::Group group;
    group.id = std::stoi(PQgetvalue(res, i, 0));
    group.name = PQgetvalue(res, i, 1);
    group.ownerId = std::stoi(PQgetvalue(res, i, 2));
    groups.push_back(group);
  }
  
  PQclear(res);
  return groups;
}

std::vector<data::User> PostgresPersistence::get_group_members(int group_id) {
  std::vector<data::User> members;
  
  std::string query = "SELECT u.id, u.username, u.password_hash, u.phone, "
                     "u.is_online, u.last_seen "
                     "FROM users u JOIN group_members gm ON u.id = gm.user_id "
                     "WHERE gm.group_id = " + std::to_string(group_id)
                     + " ORDER BY u.username";
  
  PGresult* res = db->execute(query);
  if (!res) {
    return members;
  }
  
  int rows = PQntuples(res);
  for (int i = 0; i < rows; i++) {
    data::User user;
    user.id = std::stoi(PQgetvalue(res, i, 0));
    user.username = PQgetvalue(res, i, 1);
    user.password_hash = PQgetvalue(res, i, 2);
    user.phone = PQgetvalue(res, i, 3);
    user.is_online = (strcmp(PQgetvalue(res, i, 4), "t") == 0);
    user.last_seen = PQgetisnull(res, i, 5) ? 0 : std::stoll(PQgetvalue(res, i, 5));
    members.push_back(user);
  }
  
  PQclear(res);
  return members;
}

bool PostgresPersistence::is_group_member(int group_id, int user_id) {
  std::string query = "SELECT 1 FROM group_members WHERE group_id = "
                     + std::to_string(group_id) + " AND user_id = "
                     + std::to_string(user_id);
  
  PGresult* res = db->execute(query);
  if (!res) {
    return false;
  }
  
  bool result = PQntuples(res) > 0;
  PQclear(res);
  return result;
}

// ====================================================================
// GROUP MESSAGE OPERATIONS
// ====================================================================

bool PostgresPersistence::save_group_message(int group_id, int user_id,
                                            const std::string& content,
                                            long long timestamp) {
  std::string query = "INSERT INTO group_messages (group_id, user_id, content, "
                     "timestamp) VALUES (" + std::to_string(group_id) + ", "
                     + std::to_string(user_id) + ", '"
                     + escape_string(content) + "', to_timestamp("
                     + std::to_string(timestamp / 1000.0) + "))";
  
  PGresult* res = db->execute(query);
  if (!res) {
    return false;
  }
  
  PQclear(res);
  return true;
}

std::vector<data::GroupMessage> PostgresPersistence::get_group_messages(int group_id,
                                                                        int limit) {
  std::vector<data::GroupMessage> messages;
  
  std::string query = "SELECT id, group_id, user_id, content, "
                     "(EXTRACT(EPOCH FROM timestamp) * 1000)::BIGINT "
                     "FROM group_messages WHERE group_id = "
                     + std::to_string(group_id)
                     + " ORDER BY timestamp DESC LIMIT " + std::to_string(limit);
  
  PGresult* res = db->execute(query);
  if (!res) {
    return messages;
  }
  
  int rows = PQntuples(res);
  for (int i = 0; i < rows; i++) {
    data::GroupMessage msg;
    msg.id = std::stoi(PQgetvalue(res, i, 0));
    msg.group_id = std::stoi(PQgetvalue(res, i, 1));
    msg.user_id = std::stoi(PQgetvalue(res, i, 2));
    msg.content = PQgetvalue(res, i, 3);
    msg.timestamp = std::stoll(PQgetvalue(res, i, 4));
    messages.push_back(msg);
  }
  
  PQclear(res);
  
  // Reverse to get chronological order
  std::reverse(messages.begin(), messages.end());
  return messages;
}

// ====================================================================
// ACTIVITY LOG OPERATIONS
// ====================================================================

bool PostgresPersistence::log_activity(const std::string& log_type, int user_id,
                                      int target_user_id, const std::string& details) {
  std::string query = "INSERT INTO activity_logs (log_type, user_id, target_user_id, "
                     "details, timestamp) VALUES ('" + escape_string(log_type) + "', "
                     + std::to_string(user_id) + ", ";
  
  if (target_user_id > 0) {
    query += std::to_string(target_user_id);
  } else {
    query += "NULL";
  }
  
  // store timestamp as BIGINT milliseconds since epoch to match schema
  query += ", '" + escape_string(details) + "', (EXTRACT(EPOCH FROM NOW()) * 1000)::BIGINT)";
  
  PGresult* res = db->execute(query);
  if (!res) {
    return false;
  }
  
  PQclear(res);
  return true;
}

// ====================================================================
// OFFLINE MESSAGE OPERATIONS
// ====================================================================

bool PostgresPersistence::queue_offline_message(int to_user_id, int message_type,
                                               const std::string& payload,
                                               int from_user_id) {
  std::string query = "INSERT INTO offline_messages (to_user_id, message_type, "
                     "payload";
  
  if (from_user_id > 0) {
    query += ", from_user_id) VALUES (" + std::to_string(to_user_id) + ", "
           + std::to_string(message_type) + ", '"
           + escape_string(payload) + "', " + std::to_string(from_user_id) + ")";
  } else {
    query += ") VALUES (" + std::to_string(to_user_id) + ", "
           + std::to_string(message_type) + ", '"
           + escape_string(payload) + "')";
  }
  
  PGresult* res = db->execute(query);
  if (!res) {
    return false;
  }
  
  PQclear(res);
  return true;
}

std::vector<std::pair<int, std::string>> PostgresPersistence::get_offline_messages(int user_id) {
  std::vector<std::pair<int, std::string>> messages;
  
  std::string query = "SELECT message_type, payload FROM offline_messages "
                     "WHERE to_user_id = " + std::to_string(user_id)
                     + " AND delivered = FALSE ORDER BY created_at";
  
  PGresult* res = db->execute(query);
  if (!res) {
    return messages;
  }
  
  int rows = PQntuples(res);
  for (int i = 0; i < rows; i++) {
    int msg_type = std::stoi(PQgetvalue(res, i, 0));
    std::string payload = PQgetvalue(res, i, 1);
    messages.push_back({msg_type, payload});
  }
  
  PQclear(res);
  return messages;
}

bool PostgresPersistence::mark_offline_messages_delivered(int user_id) {
  std::string query = "UPDATE offline_messages SET delivered = TRUE WHERE to_user_id = "
                     + std::to_string(user_id);
  
  PGresult* res = db->execute(query);
  if (!res) {
    return false;
  }
  
  PQclear(res);
  return true;
}

} // namespace persistence
