CREATE TABLE users (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  email TEXT NOT NULL UNIQUE,
  display_name TEXT NOT NULL,
  post_count INTEGER NOT NULL DEFAULT 0 CHECK (post_count >= 0),
  normalized_email TEXT GENERATED ALWAYS AS (lower(email)) STORED
) STRICT;
