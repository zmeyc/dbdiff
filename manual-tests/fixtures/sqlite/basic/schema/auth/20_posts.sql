CREATE TABLE posts (
  id INTEGER PRIMARY KEY,
  user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  title TEXT NOT NULL,
  body TEXT NOT NULL DEFAULT ''
) STRICT;

CREATE INDEX posts_user_title_idx ON posts (user_id, title) WHERE title <> '';

CREATE TRIGGER posts_increment_user_count
AFTER INSERT ON posts
BEGIN
  UPDATE users SET post_count = post_count + 1 WHERE id = NEW.user_id;
END;
