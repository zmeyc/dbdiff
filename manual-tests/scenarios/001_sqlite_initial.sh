#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
source "${SCRIPT_DIR}/../lib/common.sh"

case "${1:-}" in
  --describe) printf '%s\n' 'Initial SQLite create, dry-run, apply, direct replay, and no-op regeneration'; exit 0 ;;
  --groups) printf '%s\n' 'ci-safe full'; exit 0 ;;
esac

manual_init "001_sqlite_initial"
require_command sqlite3
require_dbdiff_command create
require_dbdiff_command apply
require_dbdiff_command status

project="${DBDIFF_MANUAL_RUN_ROOT}/project"
database="${DBDIFF_MANUAL_RUN_ROOT}/target.sqlite"
replay="${DBDIFF_MANUAL_RUN_ROOT}/replay.sqlite"
copy_fixture "sqlite/basic" "${project}"
mkdir -p "${project}/migrations"
write_sqlite_config "${project}/dbdiff.yaml"
export DBDIFF_MANUAL_DATABASE_URL="sqlite:${database}"

manual_step "WHEN creating an initial migration with an absent target"
assert_path_absent "${database}"
run_ok "create initial SQLite migration" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" create --name initial \
  --allow-hazard WRITE_LOCK
assert_path_absent "${database}"
assert_equals "1" "$(migration_count "${project}/migrations")" "initial migration count"
migration="$(only_migration_file "${project}/migrations")"
assert_contains "${migration}" '-- dbdiff: backend=sqlite'
assert_contains "${migration}" 'BEGIN IMMEDIATE'
assert_contains "${migration}" 'COMMIT'

manual_step "THEN dry-run leaves the missing target untouched"
run_ok "dry-run initial apply" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" apply --dry-run --create-database
assert_path_absent "${database}"

manual_step "WHEN applying to a newly created SQLite database"
run_ok "apply initial migration" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" apply --create-database
assert_file_exists "${database}"
assert_sqlite_scalar "${database}" \
  "SELECT count(*) FROM sqlite_schema WHERE type='table' AND name IN ('users','posts','application_metadata');" \
  "3"
run_ok "report converged status" "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" status

manual_step "THEN the saved SQL is directly executable"
sqlite3 -batch "${replay}" <"${migration}"
assert_sqlite_scalar "${replay}" \
  "SELECT count(*) FROM sqlite_schema WHERE type='table' AND name='users';" "1"

manual_step "THEN regenerating an unchanged schema creates no file"
before_count="$(migration_count "${project}/migrations")"
run_ok "create from unchanged master" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" create --name unchanged
assert_equals "${before_count}" "$(migration_count "${project}/migrations")" \
  "unchanged migration count"

manual_pass
