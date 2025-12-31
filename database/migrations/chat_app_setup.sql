-- Chat application bootstrap script.
-- Run this from a superuser/postgres account. It creates the database, role,
-- schema, and a small set of seed data that can be used to test the server.
--
-- Example:
--   psql -v ON_ERROR_STOP=1 -h localhost -p 5432 -U postgres -f tools/chat_app_setup.sql
-- Or with the provided app credentials:
--   PGPASSWORD=chat_app_password psql "host=localhost port=5432 user=chat_app_user dbname=postgres" -v ON_ERROR_STOP=1 -f tools/chat_app_setup.sql

-- Part 1: Administrative tasks (run as superuser)
-- =============================================================================
\echo '[ADMIN] Starting database and role creation...'

-- Create the application role if missing.
DO $$
BEGIN
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'chat_app_user') THEN
        CREATE ROLE chat_app_user LOGIN PASSWORD 'chat_app_password';
        RAISE NOTICE '[ADMIN] Role ''chat_app_user'' created.';
    ELSE
        RAISE NOTICE '[ADMIN] Role ''chat_app_user'' already exists.';
    END IF;
END
$$;

-- Create the database if missing.
SELECT 'CREATE DATABASE chat_app OWNER chat_app_user ENCODING ''UTF8'' TEMPLATE template0;'
WHERE NOT EXISTS (SELECT FROM pg_database WHERE datname = 'chat_app')
\gexec
\echo '[ADMIN] Database ''chat_app'' created and owned by ''chat_app_user''.'
\echo '[ADMIN] Administrative tasks complete.'
\echo '---'

\echo '[USER] Switching connection to database ''chat_app'' as user ''chat_app_user''...'
\connect chat_app chat_app_user localhost
\echo '[USER] Connection successful. Now creating schema objects as ''chat_app_user''...'

GRANT ALL ON SCHEMA public TO chat_app_user;

-- Core schema.
CREATE TABLE IF NOT EXISTS users (
    id SERIAL PRIMARY KEY,
    username VARCHAR(50) UNIQUE NOT NULL,
    password_hash VARCHAR(255) NOT NULL,
    phone VARCHAR(20),
    is_online BOOLEAN DEFAULT FALSE,
    last_seen TIMESTAMP DEFAULT NOW(),
    created_at TIMESTAMP DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS friends (
    id SERIAL PRIMARY KEY,
    user_id_1 INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    user_id_2 INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at TIMESTAMP DEFAULT NOW(),
    CONSTRAINT unique_friendship UNIQUE(user_id_1, user_id_2),
    CONSTRAINT user_order_check CHECK (user_id_1 < user_id_2)
);

CREATE TABLE IF NOT EXISTS friend_requests (
    id SERIAL PRIMARY KEY,
    from_user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    to_user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    status VARCHAR(20) DEFAULT 'PENDING' CHECK (status IN ('PENDING', 'ACCEPTED', 'DECLINED')),
    created_at TIMESTAMP DEFAULT NOW(),
    updated_at TIMESTAMP DEFAULT NOW(),
    CONSTRAINT unique_friend_request UNIQUE(from_user_id, to_user_id)
);

CREATE TABLE IF NOT EXISTS messages (
    id SERIAL PRIMARY KEY,
    from_user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    to_user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    content TEXT NOT NULL,
    timestamp TIMESTAMP DEFAULT NOW(),
    is_read BOOLEAN DEFAULT FALSE,
    created_at TIMESTAMP DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS groups (
    id SERIAL PRIMARY KEY,
    name VARCHAR(100) NOT NULL,
    creator_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at TIMESTAMP DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS group_members (
    id SERIAL PRIMARY KEY,
    group_id INTEGER NOT NULL REFERENCES groups(id) ON DELETE CASCADE,
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    joined_at TIMESTAMP DEFAULT NOW(),
    CONSTRAINT unique_group_membership UNIQUE(group_id, user_id)
);

CREATE TABLE IF NOT EXISTS group_invites (
    id SERIAL PRIMARY KEY,
    group_id INTEGER NOT NULL REFERENCES groups(id) ON DELETE CASCADE,
    from_user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    to_user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    status VARCHAR(20) DEFAULT 'PENDING' CHECK (status IN ('PENDING', 'ACCEPTED', 'DECLINED')),
    created_at TIMESTAMP DEFAULT NOW(),
    CONSTRAINT unique_group_invite UNIQUE (group_id, to_user_id)
);

CREATE TABLE IF NOT EXISTS group_messages (
    id SERIAL PRIMARY KEY,
    group_id INTEGER NOT NULL REFERENCES groups(id) ON DELETE CASCADE,
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    content TEXT NOT NULL,
    timestamp TIMESTAMP DEFAULT NOW(),
    created_at TIMESTAMP DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS activity_logs (
    id SERIAL PRIMARY KEY,
    log_type VARCHAR(50) NOT NULL,
    user_id INTEGER REFERENCES users(id) ON DELETE SET NULL,
    target_user_id INTEGER REFERENCES users(id) ON DELETE SET NULL,
    details TEXT,
    created_at TIMESTAMP DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS offline_messages (
    id SERIAL PRIMARY KEY,
    to_user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    message_type INTEGER NOT NULL,
    payload TEXT NOT NULL,
    from_user_id INTEGER REFERENCES users(id) ON DELETE CASCADE,
    delivered BOOLEAN DEFAULT FALSE,
    created_at TIMESTAMP DEFAULT NOW()
);

CREATE OR REPLACE VIEW v_user_friends AS
SELECT user_id_1 AS user_id, user_id_2 AS friend_id FROM friends
UNION ALL
SELECT user_id_2 AS user_id, user_id_1 AS friend_id FROM friends;

-- Pending friend requests for quick lookups.
CREATE OR REPLACE VIEW v_pending_requests AS
SELECT id, from_user_id, to_user_id, status, created_at, updated_at
FROM friend_requests
WHERE status = 'PENDING';

CREATE INDEX IF NOT EXISTS idx_users_username ON users(username);
CREATE INDEX IF NOT EXISTS idx_users_online ON users(is_online);
CREATE INDEX IF NOT EXISTS idx_friends_user1 ON friends(user_id_1);
CREATE INDEX IF NOT EXISTS idx_friends_user2 ON friends(user_id_2);
CREATE INDEX IF NOT EXISTS idx_friend_requests_to ON friend_requests(to_user_id, status);
CREATE INDEX IF NOT EXISTS idx_friend_requests_from ON friend_requests(from_user_id, status);
CREATE INDEX IF NOT EXISTS idx_messages_to ON messages(to_user_id, timestamp);
CREATE INDEX IF NOT EXISTS idx_messages_from ON messages(from_user_id, timestamp);
CREATE INDEX IF NOT EXISTS idx_group_members_group ON group_members(group_id);
CREATE INDEX IF NOT EXISTS idx_group_members_user ON group_members(user_id);
CREATE INDEX IF NOT EXISTS idx_group_messages_group ON group_messages(group_id, timestamp);
CREATE INDEX IF NOT EXISTS idx_offline_messages_user ON offline_messages(to_user_id, delivered);
CREATE INDEX IF NOT EXISTS idx_group_invites_to ON group_invites(to_user_id, status);
CREATE INDEX IF NOT EXISTS idx_group_invites_group ON group_invites(group_id);

-- Seed data for quick testing (idempotent).
WITH inserted AS (
    INSERT INTO users (username, password_hash, phone, is_online)
    VALUES
        ('dat', 'c273fa7505428a152554ab1248060934d8398ee3c5097410c92e4a4cf6986011', '123456789', TRUE),
        ('alice', 'c273fa7505428a152554ab1248060934d8398ee3c5097410c92e4a4cf6986011', '987654321', TRUE),
        ('bob', 'c273fa7505428a152554ab1248060934d8398ee3c5097410c92e4a4cf6986011', '555000111', FALSE),
        ('carol', 'c273fa7505428a152554ab1248060934d8398ee3c5097410c92e4a4cf6986011', '555000222', FALSE)
    ON CONFLICT (username) DO NOTHING
    RETURNING id, username
),
all_users AS (
    SELECT id, username FROM inserted
    UNION ALL
    SELECT id, username FROM users WHERE username IN ('dat', 'alice', 'bob', 'carol')
),
user_pairs AS (
    SELECT DISTINCT
        LEAST(u1.id, u2.id) AS user_id_1,
        GREATEST(u1.id, u2.id) AS user_id_2
    FROM (VALUES ('dat', 'alice'), ('alice', 'bob'), ('bob', 'carol')) AS p(u1name, u2name)
    JOIN all_users u1 ON u1.username = p.u1name
    JOIN all_users u2 ON u2.username = p.u2name
)
INSERT INTO friends (user_id_1, user_id_2)
SELECT user_id_1, user_id_2 FROM user_pairs
ON CONFLICT DO NOTHING;

WITH u AS (
    SELECT username, id FROM users WHERE username IN ('dat', 'alice', 'bob', 'carol')
)
INSERT INTO friend_requests (from_user_id, to_user_id, status)
SELECT
    (SELECT id FROM u WHERE username = 'dat'),
    (SELECT id FROM u WHERE username = 'carol'),
    'PENDING'
ON CONFLICT DO NOTHING;

WITH u AS (
    SELECT username, id FROM users WHERE username IN ('dat', 'alice', 'bob', 'carol')
),
g AS (
    SELECT id FROM groups WHERE name = 'Study Group'
)
INSERT INTO group_invites (group_id, from_user_id, to_user_id, status)
VALUES
    ((SELECT id FROM g), (SELECT id FROM u WHERE username = 'alice'), (SELECT id FROM u WHERE username = 'carol'), 'PENDING')
ON CONFLICT DO NOTHING;

WITH u AS (
    SELECT username, id FROM users WHERE username IN ('dat', 'alice', 'bob', 'carol')
),
g AS (
    INSERT INTO groups (name, creator_id)
    VALUES ('Study Group', (SELECT id FROM u WHERE username = 'alice'))
    ON CONFLICT DO NOTHING
    RETURNING id
),
group_row AS (
    SELECT id FROM g
    UNION ALL
    SELECT id FROM groups WHERE name = 'Study Group'
),
members AS (
    SELECT (SELECT id FROM group_row) AS group_id, id AS user_id FROM u
)
INSERT INTO group_members (group_id, user_id)
SELECT group_id, user_id FROM members
ON CONFLICT DO NOTHING;

WITH u AS (
    SELECT username, id FROM users WHERE username IN ('dat', 'alice', 'bob', 'carol')
),
g AS (
    SELECT id FROM groups WHERE name = 'Study Group'
)
INSERT INTO messages (from_user_id, to_user_id, content)
VALUES
    ((SELECT id FROM u WHERE username = 'dat'), (SELECT id FROM u WHERE username = 'alice'), 'Hey Alice, are we still on for tonight?'),
    ((SELECT id FROM u WHERE username = 'alice'), (SELECT id FROM u WHERE username = 'dat'), 'Yes! See you at 7.'),
    ((SELECT id FROM u WHERE username = 'bob'), (SELECT id FROM u WHERE username = 'carol'), 'Just sent over the files.')
ON CONFLICT DO NOTHING;

WITH u AS (
    SELECT username, id FROM users WHERE username IN ('dat', 'alice', 'bob', 'carol')
),
g AS (
    SELECT id FROM groups WHERE name = 'Study Group'
)
INSERT INTO group_messages (group_id, user_id, content)
VALUES
    ((SELECT id FROM g), (SELECT id FROM u WHERE username = 'alice'), 'Welcome to the study group!'),
    ((SELECT id FROM g), (SELECT id FROM u WHERE username = 'bob'), 'I will share notes in a bit.'),
    ((SELECT id FROM g), (SELECT id FROM u WHERE username = 'carol'), 'Does anyone have the assignment PDF?')
ON CONFLICT DO NOTHING;

-- Quick stats to confirm the import when run via psql.
\echo 'Users:'
SELECT username, phone, is_online, created_at FROM users ORDER BY username;

\echo 'Friendships:'
SELECT u1.username AS user_a, u2.username AS user_b
FROM friends f
JOIN users u1 ON u1.id = f.user_id_1
JOIN users u2 ON u2.id = f.user_id_2
ORDER BY 1, 2;

\echo 'Groups and members:'
SELECT g.name, u.username
FROM group_members gm
JOIN groups g ON g.id = gm.group_id
JOIN users u ON u.id = gm.user_id
ORDER BY g.name, u.username;
