CREATE TABLE users (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  email TEXT NOT NULL UNIQUE,
  display_name TEXT,
  normalized_email TEXT GENERATED ALWAYS AS (lower(email)) STORED
) STRICT;

CREATE TABLE posts (
  id INTEGER PRIMARY KEY,
  user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  title TEXT NOT NULL
) STRICT;

CREATE INDEX users_display_name_idx ON users(display_name) WHERE display_name IS NOT NULL;

CREATE VIEW active_user_names AS
SELECT id, display_name FROM users;

CREATE TRIGGER users_validate_email
BEFORE INSERT ON users
WHEN instr(NEW.email, '@') = 0
BEGIN
  SELECT RAISE(ABORT, 'invalid email');
END;
