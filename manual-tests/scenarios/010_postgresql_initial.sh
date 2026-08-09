#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
source "${SCRIPT_DIR}/../lib/common.sh"

case "${1:-}" in
  --describe) printf '%s\n' 'PostgreSQL history-to-master creation with an unreachable target and direct replay'; exit 0 ;;
  --groups) printf '%s\n' 'docker-smoke full'; exit 0 ;;
esac

manual_init "010_postgresql_initial"
require_docker_opt_in
require_dbdiff_command create
require_dbdiff_command apply
require_dbdiff_command status

postgres_version="${DBDIFF_MANUAL_POSTGRES_VERSION:-18}"
start_postgres_container "target" "${postgres_version}"
target_id="${DBDIFF_MANUAL_CONTAINER_ID}"
target_database="${DBDIFF_MANUAL_CONTAINER_DATABASE}"
target_dsn="${DBDIFF_MANUAL_CONTAINER_DSN}"

start_postgres_container "scratch" "${postgres_version}"
scratch_dsn="${DBDIFF_MANUAL_CONTAINER_DSN}"

project="${DBDIFF_MANUAL_RUN_ROOT}/project"
copy_fixture "postgresql/basic" "${project}"
mkdir -p "${project}/migrations"
write_postgresql_config "${project}/dbdiff.yaml"
export DBDIFF_MANUAL_SCRATCH_URL="${scratch_dsn}"
export DBDIFF_MANUAL_DATABASE_URL='postgresql://dbdiff:unreachable@127.0.0.1:1/must_not_connect'

manual_step "WHEN create is given an unreachable target locator"
run_ok "create PostgreSQL initial migration" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" create --name initial \
  --allow-hazard WRITE_LOCK
assert_equals "0" \
  "$(docker exec "${target_id}" psql --username dbdiff --dbname "${target_database}" --tuples-only --no-align \
    --command "SELECT count(*) FROM pg_namespace WHERE nspname='app';")" \
  "target schemas before apply"
migration="$(only_migration_file "${project}/migrations")"
assert_contains "${migration}" '-- dbdiff: backend=postgresql'
assert_contains "${migration}" 'BEGIN'
assert_contains "${migration}" 'COMMIT'

manual_step "WHEN dry-run and apply use the real disposable target"
export DBDIFF_MANUAL_DATABASE_URL="${target_dsn}"
run_ok "dry-run PostgreSQL apply" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" apply --dry-run
assert_equals "0" \
  "$(docker exec "${target_id}" psql --username dbdiff --dbname "${target_database}" --tuples-only --no-align \
    --command "SELECT count(*) FROM pg_namespace WHERE nspname='app';")" \
  "target schemas after dry-run"
run_ok "apply PostgreSQL migration" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" apply
assert_equals "0" \
  "$(docker exec "${target_id}" psql --username dbdiff --dbname "${target_database}" --tuples-only --no-align \
    --command "SELECT count(*) FROM app.users;")" \
  "application rows after schema apply"

# The table should be empty; query schema objects separately to avoid depending on application rows.
assert_equals "1" \
  "$(docker exec "${target_id}" psql --username dbdiff --dbname "${target_database}" --tuples-only --no-align \
    --command "SELECT count(*) FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='app' AND c.relname='users';")" \
  "managed users table count"
run_ok "report converged PostgreSQL status" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" status

manual_step "THEN the saved migration executes directly through psql"
docker exec "${target_id}" createdb --username dbdiff dbdiff_manual_replay
direct_log="$(mktemp "${DBDIFF_MANUAL_RUN_ROOT}/direct-psql.log.XXXXXX")"
if ! docker exec --interactive "${target_id}" psql --username dbdiff --dbname dbdiff_manual_replay \
  --set ON_ERROR_STOP=1 <"${migration}" >"${direct_log}" 2>&1; then
  redact_output <"${direct_log}" >&2
  manual_fail "direct psql replay failed"
fi
assert_equals "1" \
  "$(docker exec "${target_id}" psql --username dbdiff --dbname dbdiff_manual_replay --tuples-only --no-align \
    --command "SELECT count(*) FROM pg_namespace WHERE nspname='app';")" \
  "direct replay app schema count"

before_count="$(migration_count "${project}/migrations")"
run_ok "create unchanged PostgreSQL schema" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" create --name unchanged
assert_equals "${before_count}" "$(migration_count "${project}/migrations")" \
  "unchanged PostgreSQL migration count"

manual_pass
