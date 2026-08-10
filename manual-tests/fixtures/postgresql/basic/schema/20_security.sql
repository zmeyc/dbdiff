CREATE UNIQUE INDEX sessions_lookup_idx
  ON app.sessions USING btree (lower(token), user_id DESC)
  INCLUDE (payload)
  NULLS NOT DISTINCT
  WHERE revoked_at IS NULL;

ALTER TABLE app.sessions ENABLE ROW LEVEL SECURITY;
ALTER TABLE app.sessions FORCE ROW LEVEL SECURITY;

CREATE POLICY sessions_visible
  ON app.sessions
  AS PERMISSIVE
  FOR SELECT
  TO PUBLIC
  USING (revoked_at IS NULL);
