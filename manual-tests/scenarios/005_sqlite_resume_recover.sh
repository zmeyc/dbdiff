#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
source "${SCRIPT_DIR}/../lib/common.sh"

case "${1:-}" in
  --describe) printf '%s\n' 'SQLite failed suffix editing, immutable completed prefix, resume, and recovery'; exit 0 ;;
  --groups) printf '%s\n' 'ci-safe full'; exit 0 ;;
esac

manual_init "005_sqlite_resume_recover"
require_command sqlite3
require_dbdiff_command create
require_dbdiff_command apply
require_dbdiff_command recover

project="${DBDIFF_MANUAL_RUN_ROOT}/project"
database="${DBDIFF_MANUAL_RUN_ROOT}/target.sqlite"
copy_fixture "sqlite/basic" "${project}"
mkdir -p "${project}/migrations"
write_sqlite_config "${project}/dbdiff.yaml"
export DBDIFF_MANUAL_DATABASE_URL="sqlite:${database}"

run_ok "create resume baseline" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" create --name resume_baseline \
  --allow-hazard WRITE_LOCK
run_ok "apply resume baseline" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" apply --create-database
sqlite3 -batch "${database}" \
  "INSERT INTO users(email, display_name) VALUES ('taken@example.test', 'Existing');"

cat >>"${project}/schema/00_meta.sql" <<'SQL'

CREATE TABLE audit_log (
  id INTEGER PRIMARY KEY,
  message TEXT NOT NULL
) STRICT;
SQL
run_ok "create resumable migration" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" create --name resume_suffix \
  --allow-hazard WRITE_LOCK
migration="$(latest_migration_file "${project}/migrations")"
cat >>"${migration}" <<'SQL'

BEGIN IMMEDIATE;
INSERT INTO users(email, display_name) VALUES ('taken@example.test', 'Conflict');
COMMIT;
SQL
failed_revision="${DBDIFF_MANUAL_RUN_ROOT}/failed-revision.sql"
cp "${migration}" "${failed_revision}"

manual_step "WHEN a later transaction fails after the schema unit commits"
run_fails "apply migration with conflicting suffix" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" apply
assert_sqlite_scalar "${database}" \
  "SELECT count(*) FROM sqlite_schema WHERE type='table' AND name='audit_log';" "1"

manual_step "THEN editing the completed unit is rejected"
sed '0,/CREATE TABLE/{s/CREATE TABLE/CREATE  TABLE/;}' "${failed_revision}" >"${migration}"
run_fails "resume with changed completed prefix" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" apply --resume

manual_step "WHEN only the incomplete suffix is corrected"
sed 's/taken@example.test/recovered@example.test/g' "${failed_revision}" >"${migration}"
run_ok "resume corrected suffix" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" apply --resume
assert_sqlite_scalar "${database}" \
  "SELECT count(*) FROM users WHERE email='recovered@example.test';" "1"

manual_step "THEN database-stored revisions remain recoverable"
version="$(basename -- "${migration}" .sql)"
run_ok "list stored revisions" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" recover "${version}" --list
grep -Eq "^${version}"$'\t[0-9]+\t[0-9a-f]{64}$' "${DBDIFF_MANUAL_LAST_LOG}" ||
  manual_fail "recover --list did not print version, ordinal, and stored revision hash"
run_ok "recover current SQL to stdout" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" recover "${version}"
assert_contains "${DBDIFF_MANUAL_LAST_LOG}" '-- dbdiff: backend=sqlite'

chmod -R a-w "${project}/migrations"
run_ok "recover with a read-only migration directory" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" recover "${version}"

manual_pass
