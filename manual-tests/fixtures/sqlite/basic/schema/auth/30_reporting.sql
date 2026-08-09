CREATE VIEW user_post_summary AS
SELECT users.id, users.email, users.post_count
FROM users;
