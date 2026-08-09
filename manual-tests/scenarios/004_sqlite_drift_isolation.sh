#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
source "${SCRIPT_DIR}/../lib/common.sh"

case "${1:-}" in
  --describe) printf '%s\n' 'SQLite live drift blocks apply but cannot influence migration generation'; exit 0 ;;
  --groups) printf '%s\n' 'ci-safe full'; exit 0 ;;
esac

manual_init "004_sqlite_drift_isolation"
require_command sqlite3
require_dbdiff_command create
require_dbdiff_command apply
require_dbdiff_command status

project="${DBDIFF_MANUAL_RUN_ROOT}/project"
database="${DBDIFF_MANUAL_RUN_ROOT}/target.sqlite"
copy_fixture "sqlite/basic" "${project}"
mkdir -p "${project}/migrations"
write_sqlite_config "${project}/dbdiff.yaml"
export DBDIFF_MANUAL_DATABASE_URL="sqlite:${database}"

run_ok "create drift baseline" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" create --name drift_baseline \
  --allow-hazard WRITE_LOCK
run_ok "apply drift baseline" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" apply --create-database

manual_step "WHEN the live target is changed outside migration history"
sqlite3 -batch "${database}" 'CREATE TABLE manual_live_drift(id INTEGER PRIMARY KEY);'
run_status 2 "report live drift" "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" status
run_fails "apply while live drift exists" "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" apply

manual_step "THEN create ignores the live target completely"
before_count="$(migration_count "${project}/migrations")"
before_database_hash="$(sha256_file "${database}")"
run_ok "create from unchanged history and master" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" create --name must_ignore_live_drift
assert_equals "${before_count}" "$(migration_count "${project}/migrations")" \
  "migration count with live drift"
assert_equals "${before_database_hash}" "$(sha256_file "${database}")" \
  "target bytes after create"
assert_sqlite_scalar "${database}" \
  "SELECT count(*) FROM sqlite_schema WHERE type='table' AND name='manual_live_drift';" "1"

manual_pass
