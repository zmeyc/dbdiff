#!/usr/bin/env bash

set -Eeuo pipefail

DBDIFF_CLI_BIN="${1:-${DBDIFF_CLI_BIN:-}}"
DBDIFF_CLI_KEEP="${DBDIFF_CLI_KEEP:-0}"
DBDIFF_CLI_RUN_ROOT=""
DBDIFF_CLI_LAST_LOG=""

fail() {
  printf 'CLI lifecycle integration: %s\n' "$*" >&2
  return 1
}

usage() {
  cat <<'EOF'
Usage: tests/integration/cli_lifecycle.sh /path/to/dbdiff

Run the disposable SQLite create/apply/status/recover lifecycle. The executable
may instead be supplied through DBDIFF_CLI_BIN. Set DBDIFF_CLI_KEEP=1 to retain
the temporary workspace.
EOF
}

skip() {
  printf 'SKIP: %s\n' "$*"
  exit 77
}

cleanup() {
  local status="${1:-$?}"
  trap - EXIT INT TERM
  if [[ -n "${DBDIFF_CLI_RUN_ROOT}" && -d "${DBDIFF_CLI_RUN_ROOT}" ]]; then
    if [[ "${DBDIFF_CLI_KEEP}" == "1" ]]; then
      printf 'Retained CLI lifecycle workspace: %s\n' "${DBDIFF_CLI_RUN_ROOT}"
    else
      local parent name expected_parent
      parent="$(CDPATH= cd -- "$(dirname -- "${DBDIFF_CLI_RUN_ROOT}")" && pwd -P)"
      expected_parent="$(CDPATH= cd -- "${TMPDIR:-/tmp}" && pwd -P)"
      name="$(basename -- "${DBDIFF_CLI_RUN_ROOT}")"
      if [[ "${parent}" != "${expected_parent}" || "${name}" != dbdiff-cli-lifecycle.* ||
            -L "${DBDIFF_CLI_RUN_ROOT}" ]]; then
        printf 'Refusing to remove unexpected integration path: %s\n' \
          "${DBDIFF_CLI_RUN_ROOT}" >&2
        return 1
      fi
      rm -rf -- "${DBDIFF_CLI_RUN_ROOT}"
    fi
  fi
  return "${status}"
}

handle_signal() {
  local status="$1"
  cleanup "${status}" || true
  exit "${status}"
}

run_ok() {
  local label="$1"
  shift
  DBDIFF_CLI_LAST_LOG="$(mktemp "${DBDIFF_CLI_RUN_ROOT}/command.log.XXXXXX")"
  if ! "$@" >"${DBDIFF_CLI_LAST_LOG}" 2>&1; then
    cat "${DBDIFF_CLI_LAST_LOG}" >&2
    fail "${label} failed"
  fi
}

run_status() {
  local expected="$1"
  local label="$2"
  shift 2
  local status
  DBDIFF_CLI_LAST_LOG="$(mktemp "${DBDIFF_CLI_RUN_ROOT}/command.log.XXXXXX")"
  if "$@" >"${DBDIFF_CLI_LAST_LOG}" 2>&1; then
    status=0
  else
    status=$?
  fi
  if [[ "${status}" -ne "${expected}" ]]; then
    cat "${DBDIFF_CLI_LAST_LOG}" >&2
    fail "${label} exited with ${status}; expected ${expected}"
  fi
}

migration_count() {
  find "$1" -maxdepth 1 -type f -name '*.sql' -print | wc -l | tr -d '[:space:]'
}

case "${1:-}" in
  --help|-h) usage; exit 0 ;;
esac
[[ "$#" -le 1 ]] || { usage >&2; exit 2; }

[[ -n "${DBDIFF_CLI_BIN}" ]] || fail "pass the dbdiff executable as the first argument"
[[ -x "${DBDIFF_CLI_BIN}" ]] || fail "dbdiff is not executable: ${DBDIFF_CLI_BIN}"
command -v sqlite3 >/dev/null 2>&1 || skip "sqlite3 is unavailable"
command -v mktemp >/dev/null 2>&1 || fail "mktemp is unavailable"

DBDIFF_CLI_RUN_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/dbdiff-cli-lifecycle.XXXXXX")"
DBDIFF_CLI_RUN_ROOT="$(CDPATH= cd -- "${DBDIFF_CLI_RUN_ROOT}" && pwd -P)"
trap cleanup EXIT
trap 'handle_signal 130' INT
trap 'handle_signal 143' TERM

project="${DBDIFF_CLI_RUN_ROOT}/project"
database="${DBDIFF_CLI_RUN_ROOT}/target.sqlite"
mkdir -p "${project}/schema/auth" "${project}/migrations"

cat >"${project}/dbdiff.yaml" <<'YAML'
format: 1
backend: sqlite
database_env: DBDIFF_CLI_DATABASE_URL
sources:
  - schema/dbdiff.schema
migrations: migrations
lock_timeout: 5s
statement_timeout: 1m
YAML

cat >"${project}/schema/dbdiff.schema" <<'MANIFEST'
auth/users.sql
MANIFEST

cat >"${project}/schema/auth/users.sql" <<'SQL'
CREATE TABLE users (
  id INTEGER PRIMARY KEY,
  email TEXT NOT NULL UNIQUE
) STRICT;
SQL

export DBDIFF_CLI_DATABASE_URL="sqlite:${database}"

run_ok "create initial migration" \
  "${DBDIFF_CLI_BIN}" --config "${project}/dbdiff.yaml" create --name initial \
  --allow-hazard WRITE_LOCK
[[ ! -e "${database}" ]] || fail "create opened or constructed the live target"
[[ "$(migration_count "${project}/migrations")" == "1" ]] ||
  fail "create did not save exactly one migration in config.migrations"
[[ ! -d "${project}/migrations/sqlite" ]] ||
  fail "create incorrectly added a backend subdirectory beneath config.migrations"

migration="$(find "${project}/migrations" -maxdepth 1 -type f -name '*.sql' -print)"
[[ -f "${migration}" ]] || fail "created migration is missing"
grep -Fxq -- '-- dbdiff: format=1' "${migration}" || fail "format metadata is missing"
grep -Fxq -- '-- dbdiff: backend=sqlite' "${migration}" || fail "backend metadata is missing"
grep -Fq -- 'BEGIN IMMEDIATE;' "${migration}" || fail "visible BEGIN is missing"
grep -Fq -- 'COMMIT;' "${migration}" || fail "visible COMMIT is missing"

run_status 2 "status with a missing target and pending migration" \
  "${DBDIFF_CLI_BIN}" --config "${project}/dbdiff.yaml" status
run_ok "dry-run initial apply" \
  "${DBDIFF_CLI_BIN}" --config "${project}/dbdiff.yaml" apply --dry-run --create-database
[[ ! -e "${database}" ]] || fail "dry-run constructed the live target"

run_ok "apply initial migration" \
  "${DBDIFF_CLI_BIN}" --config "${project}/dbdiff.yaml" apply --create-database
[[ "$(sqlite3 -batch -noheader "${database}" \
  "SELECT count(*) FROM sqlite_schema WHERE type='table' AND name='users';")" == "1" ]] ||
  fail "applied schema does not contain users"
run_ok "status after initial apply" \
  "${DBDIFF_CLI_BIN}" --config "${project}/dbdiff.yaml" status

version="$(basename -- "${migration}" .sql)"
recovered="${DBDIFF_CLI_RUN_ROOT}/recovered.sql"
if ! "${DBDIFF_CLI_BIN}" --config "${project}/dbdiff.yaml" recover "${version}" \
  >"${recovered}" 2>"${DBDIFF_CLI_RUN_ROOT}/recover.stderr"; then
  cat "${DBDIFF_CLI_RUN_ROOT}/recover.stderr" >&2
  fail "recover failed"
fi
cmp -- "${migration}" "${recovered}" || fail "recover did not return the exact applied SQL"

cat >"${project}/schema/audit.sql" <<'SQL'
CREATE TABLE audit_log (
  id INTEGER PRIMARY KEY,
  message TEXT NOT NULL
) STRICT;
SQL
cat >>"${project}/schema/dbdiff.schema" <<'MANIFEST'
audit.sql
MANIFEST

run_ok "create migration from a second declarative file" \
  "${DBDIFF_CLI_BIN}" --config "${project}/dbdiff.yaml" create --name next_audit \
  --allow-hazard WRITE_LOCK
[[ "$(migration_count "${project}/migrations")" == "2" ]] ||
  fail "second create did not append exactly one migration"
run_ok "apply second migration" \
  "${DBDIFF_CLI_BIN}" --config "${project}/dbdiff.yaml" apply
[[ "$(sqlite3 -batch -noheader "${database}" \
  "SELECT count(*) FROM sqlite_schema WHERE type='table' AND name='audit_log';")" == "1" ]] ||
  fail "applied schema does not contain audit_log"
run_ok "status after second apply" \
  "${DBDIFF_CLI_BIN}" --config "${project}/dbdiff.yaml" status

run_ok "create unchanged schema" \
  "${DBDIFF_CLI_BIN}" --config "${project}/dbdiff.yaml" create --name unchanged
[[ "$(migration_count "${project}/migrations")" == "2" ]] ||
  fail "unchanged schema created an additional migration"

printf 'PASS: SQLite CLI create/apply/status/recover lifecycle\n'
