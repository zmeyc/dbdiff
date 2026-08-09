CREATE TABLE app.users (
  id bigint GENERATED ALWAYS AS IDENTITY,
  email text NOT NULL,
  state text NOT NULL DEFAULT 'invited',
  created_at timestamptz NOT NULL DEFAULT CURRENT_TIMESTAMP
);
