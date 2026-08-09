#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
source "${SCRIPT_DIR}/../lib/common.sh"

case "${1:-}" in
  --describe) printf '%s\n' 'SQLite table rebuild with rows, rowids, sequence, constraints, and dependents'; exit 0 ;;
  --groups) printf '%s\n' 'ci-safe full'; exit 0 ;;
esac

manual_init "003_sqlite_rebuild_preserves_data"
require_command sqlite3
require_dbdiff_command create
require_dbdiff_command apply

project="${DBDIFF_MANUAL_RUN_ROOT}/project"
database="${DBDIFF_MANUAL_RUN_ROOT}/target.sqlite"
mkdir -p "${project}/schema" "${project}/migrations"
cp "${DBDIFF_MANUAL_ROOT}/fixtures/sqlite/rebuild/v1.sql" "${project}/schema/master.sql"
write_sqlite_config "${project}/dbdiff.yaml"
export DBDIFF_MANUAL_DATABASE_URL="sqlite:${database}"

run_ok "create rebuild baseline" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" create --name rebuild_baseline \
  --allow-hazard WRITE_LOCK
run_ok "apply rebuild baseline" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" apply --create-database
sqlite3 -batch "${database}" <<'SQL'
PRAGMA foreign_keys = ON;
INSERT INTO users(email, display_name) VALUES
  ('one@example.test', 'One'),
  ('two@example.test', 'Two');
INSERT INTO posts(id, user_id, title) VALUES (10, 2, 'kept');
SQL
before_ids="$(sqlite_scalar "${database}" 'SELECT group_concat(id, '"'"','"'"') FROM users ORDER BY id;')"
before_sequence="$(sqlite_scalar "${database}" "SELECT seq FROM sqlite_sequence WHERE name='users';")"

manual_step "WHEN master requires a checked-column table rebuild"
cp "${DBDIFF_MANUAL_ROOT}/fixtures/sqlite/rebuild/v2.sql" "${project}/schema/master.sql"
run_fails "create rebuild without hazard approvals" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" create --name rebuild_users
run_ok "create approved rebuild" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" create --name rebuild_users \
  --allow-hazard TABLE_REWRITE --allow-hazard WRITE_LOCK --allow-hazard CONSTRAINT_SCAN
migration="$(latest_migration_file "${project}/migrations")"
assert_contains "${migration}" 'BEGIN IMMEDIATE'
assert_not_contains "${migration}" 'writable_schema'

manual_step "THEN data-bearing validation and apply preserve every dependent object"
run_ok "validate and apply rebuild" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" apply --validate-data
assert_equals "${before_ids}" \
  "$(sqlite_scalar "${database}" 'SELECT group_concat(id, '"'"','"'"') FROM users ORDER BY id;')" \
  "user IDs after rebuild"
assert_equals "${before_sequence}" \
  "$(sqlite_scalar "${database}" "SELECT seq FROM sqlite_sequence WHERE name='users';")" \
  "AUTOINCREMENT sequence after rebuild"
assert_sqlite_scalar "${database}" "SELECT normalized_email FROM users WHERE id=1;" \
  "one@example.test"
assert_sqlite_scalar "${database}" "SELECT active FROM users WHERE id=2;" "1"
assert_sqlite_scalar "${database}" "PRAGMA foreign_key_check;" ""
assert_sqlite_scalar "${database}" \
  "SELECT count(*) FROM sqlite_schema WHERE name IN ('users_display_name_idx','active_user_names','users_validate_email');" \
  "3"
run_fails "trigger still rejects an invalid email" \
  sqlite3 -batch "${database}" "INSERT INTO users(email, display_name) VALUES ('invalid', 'No');"

manual_pass
