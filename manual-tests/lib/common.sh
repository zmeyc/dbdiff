#!/usr/bin/env bash

set -Eeuo pipefail

DBDIFF_MANUAL_LIB_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
DBDIFF_MANUAL_ROOT="$(CDPATH= cd -- "${DBDIFF_MANUAL_LIB_DIR}/.." && pwd -P)"
DBDIFF_REPO_ROOT="$(CDPATH= cd -- "${DBDIFF_MANUAL_ROOT}/.." && pwd -P)"
DBDIFF_BIN="${DBDIFF_BIN:-${DBDIFF_REPO_ROOT}/build/debug/dbdiff}"
DBDIFF_MANUAL_KEEP="${DBDIFF_MANUAL_KEEP:-0}"

declare -a DBDIFF_MANUAL_CONTAINERS=()
DBDIFF_MANUAL_CONTAINER_COUNT=0
DBDIFF_MANUAL_RUN_ROOT=""
DBDIFF_MANUAL_OWNS_SESSION=0
DBDIFF_MANUAL_SESSION_ROOT="${DBDIFF_MANUAL_SESSION_ROOT:-}"
DBDIFF_MANUAL_RUN_ID=""
DBDIFF_MANUAL_SCENARIO=""
DBDIFF_MANUAL_LAST_LOG=""
DBDIFF_MANUAL_CONTAINER_ID=""
DBDIFF_MANUAL_CONTAINER_DSN=""
DBDIFF_MANUAL_CONTAINER_DATABASE=""

manual_note() {
  printf '  %s\n' "$*"
}

manual_step() {
  printf '\n[%s] %s\n' "${DBDIFF_MANUAL_SCENARIO:-manual}" "$*"
}

manual_fail() {
  printf 'FAIL: %s\n' "$*" >&2
  return 1
}

require_command() {
  command -v "$1" >/dev/null 2>&1 || manual_fail "required command not found: $1"
}

require_dbdiff() {
  if [[ ! -x "${DBDIFF_BIN}" ]]; then
    manual_fail "dbdiff is not executable at ${DBDIFF_BIN}; set DBDIFF_BIN explicitly"
  fi
}

require_dbdiff_command() {
  local command_name="$1"
  local help_file
  help_file="$(mktemp "${DBDIFF_MANUAL_RUN_ROOT}/command-help.XXXXXX")"
  if ! "${DBDIFF_BIN}" "${command_name}" --help >"${help_file}" 2>&1; then
    manual_fail "dbdiff command is not available yet: ${command_name}"
  fi
  if ! grep -Fq -- "${command_name}" "${help_file}"; then
    manual_fail "dbdiff command is not advertised yet: ${command_name}"
  fi
}

redact_output() {
  sed -E \
    -e 's#(postgres(ql)?://[^:/@[:space:]]+):[^@[:space:]]+@#\1:***@#g' \
    -e 's#(password=)[^[:space:]]+#\1***#g'
}

run_ok() {
  local label="$1"
  shift
  local log_file
  log_file="$(mktemp "${DBDIFF_MANUAL_RUN_ROOT}/command.log.XXXXXX")"
  DBDIFF_MANUAL_LAST_LOG="${log_file}"
  manual_note "WHEN ${label}"
  local status
  if "$@" >"${log_file}" 2>&1; then
    return 0
  else
    status=$?
  fi
  manual_note "command output:"
  redact_output <"${log_file}" >&2
  manual_fail "${label} exited with ${status}"
}

run_ok_secret() {
  local label="$1"
  shift
  local raw_file redacted_file status
  raw_file="$(mktemp "${DBDIFF_MANUAL_RUN_ROOT}/secret.log.XXXXXX")"
  redacted_file="$(mktemp "${DBDIFF_MANUAL_RUN_ROOT}/command.log.XXXXXX")"
  chmod 600 "${raw_file}" "${redacted_file}"
  manual_note "WHEN ${label}"
  if "$@" >"${raw_file}" 2>&1; then
    status=0
  else
    status=$?
  fi
  redact_output <"${raw_file}" >"${redacted_file}"
  : >"${raw_file}"
  rm -- "${raw_file}"
  DBDIFF_MANUAL_LAST_LOG="${redacted_file}"
  if [[ "${status}" -ne 0 ]]; then
    cat "${redacted_file}" >&2
    manual_fail "${label} exited with ${status}"
  fi
}

run_fails() {
  local label="$1"
  shift
  local log_file
  log_file="$(mktemp "${DBDIFF_MANUAL_RUN_ROOT}/command.log.XXXXXX")"
  DBDIFF_MANUAL_LAST_LOG="${log_file}"
  manual_note "WHEN ${label} (expected failure)"
  if "$@" >"${log_file}" 2>&1; then
    redact_output <"${log_file}" >&2
    manual_fail "${label} unexpectedly succeeded"
  fi
}

run_status() {
  local expected="$1"
  local label="$2"
  shift 2
  local log_file status
  log_file="$(mktemp "${DBDIFF_MANUAL_RUN_ROOT}/command.log.XXXXXX")"
  DBDIFF_MANUAL_LAST_LOG="${log_file}"
  manual_note "WHEN ${label} (expected status ${expected})"
  if "$@" >"${log_file}" 2>&1; then
    status=0
  else
    status=$?
  fi
  if [[ "${status}" -ne "${expected}" ]]; then
    redact_output <"${log_file}" >&2
    manual_fail "${label} exited with ${status}; expected ${expected}"
  fi
}

assert_file_exists() {
  [[ -f "$1" ]] || manual_fail "expected file does not exist: $1"
}

assert_path_absent() {
  [[ ! -e "$1" ]] || manual_fail "expected path to remain absent: $1"
}

assert_contains() {
  local file="$1"
  local text="$2"
  grep -Fq -- "${text}" "${file}" || manual_fail "${file} does not contain: ${text}"
}

assert_not_contains() {
  local file="$1"
  local text="$2"
  if grep -Fq -- "${text}" "${file}"; then
    manual_fail "${file} unexpectedly contains: ${text}"
  fi
}

assert_equals() {
  local expected="$1"
  local actual="$2"
  local context="$3"
  [[ "${actual}" == "${expected}" ]] ||
    manual_fail "${context}: expected '${expected}', got '${actual}'"
}

migration_count() {
  local directory="$1"
  find "${directory}" -maxdepth 1 -type f -name '*.sql' -print | wc -l | tr -d '[:space:]'
}

only_migration_file() {
  local directory="$1"
  local -a files=()
  while IFS= read -r file; do
    files+=("${file}")
  done < <(find "${directory}" -maxdepth 1 -type f -name '*.sql' -print | sort)
  [[ "${#files[@]}" -eq 1 ]] || manual_fail "expected one migration in ${directory}"
  printf '%s\n' "${files[0]}"
}

latest_migration_file() {
  local directory="$1"
  local file
  file="$(find "${directory}" -maxdepth 1 -type f -name '*.sql' -print | sort | tail -n 1)"
  [[ -n "${file}" ]] || manual_fail "no migration found in ${directory}"
  printf '%s\n' "${file}"
}

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    manual_fail "sha256sum or shasum is required"
  fi
}

sqlite_scalar() {
  local database="$1"
  local sql="$2"
  sqlite3 -batch -noheader "${database}" "${sql}"
}

assert_sqlite_scalar() {
  local database="$1"
  local sql="$2"
  local expected="$3"
  local actual
  actual="$(sqlite_scalar "${database}" "${sql}")"
  assert_equals "${expected}" "${actual}" "SQLite query ${sql}"
}

copy_fixture() {
  local fixture="$1"
  local destination="$2"
  [[ -d "${DBDIFF_MANUAL_ROOT}/fixtures/${fixture}" ]] ||
    manual_fail "fixture does not exist: ${fixture}"
  mkdir -p "${destination}"
  cp -R "${DBDIFF_MANUAL_ROOT}/fixtures/${fixture}/." "${destination}/"
}

write_sqlite_config() {
  local file="$1"
  cat >"${file}" <<'YAML'
format: 1
backend: sqlite
database_env: DBDIFF_MANUAL_DATABASE_URL
sources:
  - schema
migrations: migrations
lock_timeout: 5s
statement_timeout: 1m
YAML
}

write_postgresql_config() {
  local file="$1"
  cat >"${file}" <<'YAML'
format: 1
backend: postgresql
database_env: DBDIFF_MANUAL_DATABASE_URL
sources:
  - schema
migrations: migrations
scratch:
  database_env: DBDIFF_MANUAL_SCRATCH_URL
managed_schemas:
  - public
  - app
lock_timeout: 5s
statement_timeout: 1m
YAML
}

safe_remove_tree() {
  local target="$1"
  local parent name session_root temporary_base
  [[ -n "${target}" && -d "${target}" && ! -L "${target}" ]] ||
    manual_fail "refusing to remove invalid directory: ${target}"
  parent="$(CDPATH= cd -- "$(dirname -- "${target}")" && pwd -P)"
  name="$(basename -- "${target}")"
  session_root="$(CDPATH= cd -- "${DBDIFF_MANUAL_SESSION_ROOT}" && pwd -P)"
  temporary_base="$(CDPATH= cd -- "${TMPDIR:-/tmp}" && pwd -P)"
  if [[ "${parent}" == "${session_root}" &&
        "${name}" == [0-9][0-9][0-9]_[a-z0-9_]*.* ]]; then
    :
  elif [[ "${target}" == "${session_root}" && "${parent}" == "${temporary_base}" &&
          "${name}" == dbdiff-manual-session.* ]]; then
    :
  else
    manual_fail "refusing to remove directory outside the exact manual-test roots: ${target}"
  fi
  chmod -R u+w -- "${target}" 2>/dev/null || true
  rm -rf -- "${target}"
}

random_hex() {
  require_command od
  od -An -N16 -tx1 /dev/urandom | tr -d ' \n'
}

require_docker_opt_in() {
  if [[ "${DBDIFF_MANUAL_ALLOW_DOCKER:-}" != "I_UNDERSTAND" ]]; then
    manual_fail "Docker scenarios require DBDIFF_MANUAL_ALLOW_DOCKER=I_UNDERSTAND"
  fi
  require_command docker
  docker info >/dev/null 2>&1 || manual_fail "Docker daemon is unavailable"
}

register_container() {
  local container_id="$1"
  [[ "${container_id}" =~ ^[a-f0-9]{64}$ ]] || manual_fail "invalid container ID: ${container_id}"
  DBDIFF_MANUAL_CONTAINERS+=("${container_id}")
  DBDIFF_MANUAL_CONTAINER_COUNT=$((DBDIFF_MANUAL_CONTAINER_COUNT + 1))
}

verify_manual_container() {
  local container_id="$1"
  local manual_label run_label
  manual_label="$(docker inspect --format '{{ index .Config.Labels "io.dbdiff.manual" }}' "${container_id}")"
  run_label="$(docker inspect --format '{{ index .Config.Labels "io.dbdiff.manual.run" }}' "${container_id}")"
  [[ "${manual_label}" == "true" && "${run_label}" == "${DBDIFF_MANUAL_RUN_ID}" ]] ||
    manual_fail "refusing container operation: ownership labels do not match ${container_id}"
}

start_postgres_container() {
  local role="$1"
  local version="${2:-18}"
  local password database name container_id port_binding port
  require_docker_opt_in
  [[ "${role}" =~ ^[a-z][a-z0-9_-]*$ ]] || manual_fail "invalid container role: ${role}"
  [[ "${version}" =~ ^(15|16|17|18)$ ]] || manual_fail "unsupported PostgreSQL image version: ${version}"

  password="$(random_hex)"
  database="dbdiff_manual_${DBDIFF_MANUAL_RUN_ID//-/_}_${role//-/_}"
  database="${database:0:60}"
  name="dbdiff-manual-${role}-${DBDIFF_MANUAL_RUN_ID}"
  container_id="$(docker run --detach \
    --name "${name}" \
    --label io.dbdiff.manual=true \
    --label "io.dbdiff.manual.run=${DBDIFF_MANUAL_RUN_ID}" \
    --label "io.dbdiff.manual.role=${role}" \
    --env POSTGRES_USER=dbdiff \
    --env "POSTGRES_PASSWORD=${password}" \
    --env "POSTGRES_DB=${database}" \
    --publish 127.0.0.1::5432 \
    "postgres:${version}")"
  register_container "${container_id}"
  verify_manual_container "${container_id}"

  local attempt
  for attempt in $(seq 1 60); do
    if docker exec "${container_id}" pg_isready --username dbdiff --dbname "${database}" >/dev/null 2>&1; then
      break
    fi
    if [[ "${attempt}" -eq 60 ]]; then
      docker logs "${container_id}" 2>&1 | redact_output >&2 || true
      manual_fail "PostgreSQL container did not become ready: ${container_id}"
    fi
    sleep 1
  done

  port_binding="$(docker port "${container_id}" 5432/tcp | head -n 1)"
  [[ "${port_binding}" == 127.0.0.1:* ]] || manual_fail "unexpected Docker port binding: ${port_binding}"
  port="${port_binding##*:}"
  [[ "${port}" =~ ^[0-9]+$ ]] || manual_fail "invalid Docker port: ${port}"

  DBDIFF_MANUAL_CONTAINER_ID="${container_id}"
  DBDIFF_MANUAL_CONTAINER_DATABASE="${database}"
  DBDIFF_MANUAL_CONTAINER_DSN="postgresql://dbdiff:${password}@127.0.0.1:${port}/${database}"
}

manual_cleanup() {
  local original_status="${1:-$?}"
  trap - EXIT INT TERM
  if [[ "${DBDIFF_MANUAL_KEEP}" == "1" ]]; then
    manual_note "artifacts retained at ${DBDIFF_MANUAL_RUN_ROOT}"
    if [[ "${DBDIFF_MANUAL_CONTAINER_COUNT}" -gt 0 ]]; then
      manual_note "retained container IDs: ${DBDIFF_MANUAL_CONTAINERS[*]}"
    fi
    return "${original_status}"
  fi

  local container_id
  if [[ "${DBDIFF_MANUAL_CONTAINER_COUNT}" -gt 0 ]]; then
    for container_id in "${DBDIFF_MANUAL_CONTAINERS[@]}"; do
      if docker inspect "${container_id}" >/dev/null 2>&1; then
        if verify_manual_container "${container_id}"; then
          docker rm --force "${container_id}" >/dev/null
        fi
      fi
    done
  fi
  if [[ -n "${DBDIFF_MANUAL_RUN_ROOT}" && -d "${DBDIFF_MANUAL_RUN_ROOT}" ]]; then
    safe_remove_tree "${DBDIFF_MANUAL_RUN_ROOT}"
  fi
  if [[ "${DBDIFF_MANUAL_OWNS_SESSION}" == "1" && -n "${DBDIFF_MANUAL_SESSION_ROOT}" &&
        -d "${DBDIFF_MANUAL_SESSION_ROOT}" ]]; then
    safe_remove_tree "${DBDIFF_MANUAL_SESSION_ROOT}"
  fi
  return "${original_status}"
}

manual_signal() {
  local status="$1"
  manual_cleanup "${status}" || true
  exit "${status}"
}

manual_init() {
  local scenario="$1"
  local temporary_base
  [[ "${scenario}" =~ ^[0-9]{3}_[a-z0-9_]+$ ]] || manual_fail "invalid scenario name: ${scenario}"
  require_command mktemp
  temporary_base="${TMPDIR:-/tmp}"
  [[ -d "${temporary_base}" && ! -L "${temporary_base}" ]] ||
    manual_fail "temporary directory is not safe: ${temporary_base}"

  if [[ -z "${DBDIFF_MANUAL_SESSION_ROOT}" ]]; then
    DBDIFF_MANUAL_SESSION_ROOT="$(mktemp -d "${temporary_base}/dbdiff-manual-session.XXXXXX")"
    DBDIFF_MANUAL_SESSION_ROOT="$(CDPATH= cd -- "${DBDIFF_MANUAL_SESSION_ROOT}" && pwd -P)"
    DBDIFF_MANUAL_OWNS_SESSION=1
  fi
  [[ -d "${DBDIFF_MANUAL_SESSION_ROOT}" && ! -L "${DBDIFF_MANUAL_SESSION_ROOT}" ]] ||
    manual_fail "manual-test session root is invalid"

  DBDIFF_MANUAL_SCENARIO="${scenario}"
  DBDIFF_MANUAL_RUN_ID="$(date -u +%Y%m%d%H%M%S)-$$-${RANDOM}"
  DBDIFF_MANUAL_RUN_ROOT="$(mktemp -d "${DBDIFF_MANUAL_SESSION_ROOT}/${scenario}.XXXXXX")"
  DBDIFF_MANUAL_RUN_ROOT="$(CDPATH= cd -- "${DBDIFF_MANUAL_RUN_ROOT}" && pwd -P)"
  trap manual_cleanup EXIT
  trap 'manual_signal 130' INT
  trap 'manual_signal 143' TERM
  require_dbdiff
  manual_step "GIVEN isolated workspace ${DBDIFF_MANUAL_RUN_ROOT}"
}

manual_pass() {
  manual_step "PASS"
}
