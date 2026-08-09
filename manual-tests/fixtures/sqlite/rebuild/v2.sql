CREATE TABLE users (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  email TEXT NOT NULL UNIQUE,
  display_name TEXT CHECK (display_name IS NULL OR length(display_name) > 0),
  active INTEGER NOT NULL DEFAULT 1 CHECK (active IN (0, 1)),
  normalized_email TEXT GENERATED ALWAYS AS (lower(email)) STORED
) STRICT;

CREATE TABLE posts (
  id INTEGER PRIMARY KEY,
  user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  title TEXT NOT NULL
) STRICT;

CREATE INDEX users_display_name_idx ON users(display_name) WHERE active = 1;

CREATE VIEW active_user_names AS
SELECT id, display_name FROM users WHERE active = 1;

CREATE TRIGGER users_validate_email
BEFORE INSERT ON users
WHEN instr(NEW.email, '@') = 0
BEGIN
  SELECT RAISE(ABORT, 'invalid email');
END;
