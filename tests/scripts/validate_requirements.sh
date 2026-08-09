#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(CDPATH= cd -- "${SCRIPT_DIR}/../.." && pwd -P)"
MATRIX="${REPO_ROOT}/tests/requirements.tsv"
EVIDENCE="${REPO_ROOT}/tests/evidence.tsv"
SCENARIO_DIR="${REPO_ROOT}/manual-tests/scenarios"
STRICT_EVIDENCE=0
BUILD_DIR=""
TEMP_ROOT=""

usage() {
  cat <<'EOF'
Usage: tests/scripts/validate_requirements.sh [--strict] [--build-dir DIR]

Validate the supported-versus-planned requirements matrix and its explicit
evidence map. Structural validation does not claim that automated evidence is
registered. --strict requires --build-dir and verifies every unit and
integration evidence name against the tests actually registered with CTest.

The legacy --verify-test-ids spelling is accepted as an alias for --strict.
EOF
}

fail() {
  printf 'requirements traceability: %s\n' "$*" >&2
  exit 1
}

safe_cleanup() {
  local status="${1:-$?}"
  trap - EXIT INT TERM
  if [[ -n "${TEMP_ROOT}" && -d "${TEMP_ROOT}" && ! -L "${TEMP_ROOT}" ]]; then
    local parent name expected_parent
    parent="$(CDPATH= cd -- "$(dirname -- "${TEMP_ROOT}")" && pwd -P)"
    expected_parent="$(CDPATH= cd -- "${TMPDIR:-/tmp}" && pwd -P)"
    name="$(basename -- "${TEMP_ROOT}")"
    if [[ "${parent}" != "${expected_parent}" || "${name}" != dbdiff-requirements.* ]]; then
      printf 'Refusing to remove unexpected validator path: %s\n' "${TEMP_ROOT}" >&2
      return 1
    fi
    rm -rf -- "${TEMP_ROOT}"
  fi
  return "${status}"
}

handle_signal() {
  local status="$1"
  safe_cleanup "${status}" || true
  exit "${status}"
}

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --strict|--verify-test-ids)
      STRICT_EVIDENCE=1
      shift
      ;;
    --build-dir)
      [[ "$#" -ge 2 ]] || { usage >&2; exit 2; }
      BUILD_DIR="$2"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      usage >&2
      exit 2
      ;;
  esac
done

if [[ "${STRICT_EVIDENCE}" == "1" && -z "${BUILD_DIR}" ]]; then
  fail "--strict requires --build-dir so evidence is checked against registered tests"
fi

[[ -f "${MATRIX}" && ! -L "${MATRIX}" ]] || fail "missing regular file ${MATRIX}"
[[ -f "${EVIDENCE}" && ! -L "${EVIDENCE}" ]] || fail "missing regular file ${EVIDENCE}"
[[ -d "${SCENARIO_DIR}" && ! -L "${SCENARIO_DIR}" ]] || fail "missing scenario directory"
for command in awk ctest grep mktemp sed sort; do
  command -v "${command}" >/dev/null 2>&1 || fail "${command} is required"
done

TEMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/dbdiff-requirements.XXXXXX")"
TEMP_ROOT="$(CDPATH= cd -- "${TEMP_ROOT}" && pwd -P)"
trap safe_cleanup EXIT
trap 'handle_signal 130' INT
trap 'handle_signal 143' TERM

if ! awk -F '\t' 'NF != 5 { print NR ":" NF; bad=1 } END { exit bad }' "${MATRIX}" \
  >"${TEMP_ROOT}/bad-matrix-fields"; then
  cat "${TEMP_ROOT}/bad-matrix-fields" >&2
  fail "every requirements row must contain exactly five tab-separated fields"
fi
if ! awk -F '\t' 'NF != 3 { print NR ":" NF; bad=1 } END { exit bad }' "${EVIDENCE}" \
  >"${TEMP_ROOT}/bad-evidence-fields"; then
  cat "${TEMP_ROOT}/bad-evidence-fields" >&2
  fail "every evidence row must contain exactly three tab-separated fields"
fi

expected_matrix_header=$'id\tarea\tstatus\trequired_evidence\trequirement'
IFS= read -r actual_matrix_header <"${MATRIX}"
[[ "${actual_matrix_header}" == "${expected_matrix_header}" ]] ||
  fail "unexpected requirements.tsv header"
expected_evidence_header=$'requirement_id\tkind\tname'
IFS= read -r actual_evidence_header <"${EVIDENCE}"
[[ "${actual_evidence_header}" == "${expected_evidence_header}" ]] ||
  fail "unexpected evidence.tsv header"

: >"${TEMP_ROOT}/ids"
: >"${TEMP_ROOT}/supported"
: >"${TEMP_ROOT}/supported-contracts"
: >"${TEMP_ROOT}/planned"
line_number=1
while IFS=$'\t' read -r id area status required_evidence requirement; do
  line_number=$((line_number + 1))
  [[ -n "${id}${area}${status}${required_evidence}${requirement}" ]] || continue
  [[ "${id}" =~ ^[A-Z][A-Z0-9]*-[0-9]{3}$ ]] ||
    fail "invalid requirement ID at line ${line_number}: ${id}"
  grep -Fxq -- "${id}" "${TEMP_ROOT}/ids" &&
    fail "duplicate requirement ID at line ${line_number}: ${id}"
  [[ -n "${area}" && -n "${requirement}" ]] ||
    fail "empty requirement field at line ${line_number}: ${id}"
  case "${status}" in
    supported)
      case "${required_evidence}" in
        unit|integration|unit+integration) ;;
        *) fail "invalid supported evidence contract for ${id}: ${required_evidence}" ;;
      esac
      printf '%s\t%s\n' "${id}" "${required_evidence}" \
        >>"${TEMP_ROOT}/supported-contracts"
      ;;
    planned)
      [[ "${required_evidence}" == "-" ]] ||
        fail "planned requirement ${id} must declare required_evidence as -"
      ;;
    *) fail "invalid status at line ${line_number}: ${status}" ;;
  esac
  printf '%s\n' "${id}" >>"${TEMP_ROOT}/ids"
  printf '%s\n' "${id}" >>"${TEMP_ROOT}/${status}"
done < <(tail -n +2 "${MATRIX}")
[[ -s "${TEMP_ROOT}/ids" ]] || fail "requirements matrix contains no rows"

: >"${TEMP_ROOT}/unit-evidence"
: >"${TEMP_ROOT}/integration-evidence"
: >"${TEMP_ROOT}/manual-evidence"
: >"${TEMP_ROOT}/evidence-rows"
line_number=1
while IFS=$'\t' read -r id kind name; do
  line_number=$((line_number + 1))
  [[ -n "${id}${kind}${name}" ]] || continue
  grep -Fxq -- "${id}" "${TEMP_ROOT}/ids" ||
    fail "evidence at line ${line_number} references unknown requirement ${id}"
  grep -Fxq -- "${id}" "${TEMP_ROOT}/planned" &&
    fail "planned requirement ${id} must not claim evidence"
  case "${kind}" in
    unit|integration|manual) ;;
    *) fail "invalid evidence kind at line ${line_number}: ${kind}" ;;
  esac
  [[ -n "${name}" ]] || fail "empty evidence name at line ${line_number}"
  row="${id}"$'\t'"${kind}"$'\t'"${name}"
  grep -Fxq -- "${row}" "${TEMP_ROOT}/evidence-rows" &&
    fail "duplicate evidence row at line ${line_number}: ${id} ${kind} ${name}"
  printf '%s\n' "${row}" >>"${TEMP_ROOT}/evidence-rows"
  printf '%s\t%s\n' "${id}" "${name}" >>"${TEMP_ROOT}/${kind}-evidence"
done < <(tail -n +2 "${EVIDENCE}")

while IFS=$'\t' read -r id required_evidence; do
  grep -Eq "^${id}"$'\t' "${TEMP_ROOT}/evidence-rows" ||
    fail "supported requirement ${id} has no evidence"
  case "${required_evidence}" in
    unit)
      grep -Eq "^${id}"$'\t' "${TEMP_ROOT}/unit-evidence" ||
        fail "supported requirement ${id} requires unit evidence"
      ;;
    integration)
      grep -Eq "^${id}"$'\t' "${TEMP_ROOT}/integration-evidence" ||
        fail "supported requirement ${id} requires integration evidence"
      ;;
    unit+integration)
      grep -Eq "^${id}"$'\t' "${TEMP_ROOT}/unit-evidence" ||
        fail "supported requirement ${id} requires unit evidence"
      grep -Eq "^${id}"$'\t' "${TEMP_ROOT}/integration-evidence" ||
        fail "supported requirement ${id} requires integration evidence"
      ;;
  esac
done <"${TEMP_ROOT}/supported-contracts"

: >"${TEMP_ROOT}/referenced-scenarios"
while IFS=$'\t' read -r id scenario; do
  [[ -n "${id}${scenario}" ]] || continue
  [[ "${scenario}" =~ ^[0-9]{3}_[a-z0-9_]+$ ]] ||
    fail "invalid manual scenario name for ${id}: ${scenario}"
  scenario_file="${SCENARIO_DIR}/${scenario}.sh"
  [[ -f "${scenario_file}" && ! -L "${scenario_file}" ]] ||
    fail "manual evidence for ${id} is not a regular scenario: ${scenario}"
  printf '%s\n' "${scenario}" >>"${TEMP_ROOT}/referenced-scenarios"
done <"${TEMP_ROOT}/manual-evidence"
sort -u "${TEMP_ROOT}/referenced-scenarios" >"${TEMP_ROOT}/unique-scenarios"

while IFS= read -r scenario_file; do
  scenario="$(basename -- "${scenario_file}" .sh)"
  grep -Fxq -- "${scenario}" "${TEMP_ROOT}/unique-scenarios" ||
    fail "manual scenario is not present in evidence.tsv: ${scenario}"
  grep -Fq -- 'set -Eeuo pipefail' "${scenario_file}" ||
    fail "manual scenario does not enable strict shell behavior: ${scenario}"
  grep -Fq -- 'manual_init ' "${scenario_file}" ||
    fail "manual scenario does not create an isolated workspace: ${scenario}"
  groups="$(bash "${scenario_file}" --groups)"
  [[ "${groups}" =~ (^|[[:space:]])(ci-safe|docker-smoke|full)([[:space:]]|$) ]] ||
    fail "manual scenario has no known runner group: ${scenario}"
  if [[ " ${groups} " == *' docker-smoke '* ]]; then
    grep -Fq -- 'require_docker_opt_in' "${scenario_file}" ||
      fail "Docker scenario lacks an explicit opt-in guard: ${scenario}"
  fi
done < <(find "${SCENARIO_DIR}" -maxdepth 1 -type f -name '[0-9][0-9][0-9]_*.sh' -print | sort)

grep -Fq -- 'mktemp -d' "${REPO_ROOT}/manual-tests/run.sh" ||
  fail "manual runner must create a mktemp session root"
grep -Fq -- 'mktemp -d' "${REPO_ROOT}/manual-tests/lib/common.sh" ||
  fail "manual library must create a mktemp scenario root"
grep -Fq -- 'io.dbdiff.manual.run' "${REPO_ROOT}/manual-tests/lib/common.sh" ||
  fail "manual Docker cleanup must verify a run ownership label"

if [[ -n "${BUILD_DIR}" ]]; then
  if [[ "${BUILD_DIR}" != /* ]]; then
    BUILD_DIR="${PWD}/${BUILD_DIR}"
  fi
  [[ -d "${BUILD_DIR}" ]] || fail "CTest build directory does not exist: ${BUILD_DIR}"
  BUILD_DIR="$(CDPATH= cd -- "${BUILD_DIR}" && pwd -P)"
  [[ -f "${BUILD_DIR}/CTestTestfile.cmake" ]] ||
    fail "build directory has no CTest registration: ${BUILD_DIR}"

  ctest --test-dir "${BUILD_DIR}" -N -L '^unit$' >"${TEMP_ROOT}/ctest-unit-output"
  ctest --test-dir "${BUILD_DIR}" -N -L '^integration' \
    >"${TEMP_ROOT}/ctest-integration-output"
  sed -n 's/^[[:space:]]*Test[[:space:]]*#[0-9][0-9]*: //p' \
    "${TEMP_ROOT}/ctest-unit-output" >"${TEMP_ROOT}/registered-unit"
  sed -n 's/^[[:space:]]*Test[[:space:]]*#[0-9][0-9]*: //p' \
    "${TEMP_ROOT}/ctest-integration-output" >"${TEMP_ROOT}/registered-integration"
  [[ -s "${TEMP_ROOT}/registered-unit" ]] || fail "CTest registered no unit tests"
  [[ -s "${TEMP_ROOT}/registered-integration" ]] || fail "CTest registered no integration tests"

  while IFS=$'\t' read -r id name; do
    [[ -n "${id}${name}" ]] || continue
    grep -Fxq -- "${name}" "${TEMP_ROOT}/registered-unit" ||
      fail "unit evidence for ${id} is not a registered CTest name: ${name}"
  done <"${TEMP_ROOT}/unit-evidence"
  while IFS=$'\t' read -r id name; do
    [[ -n "${id}${name}" ]] || continue
    grep -Fxq -- "${name}" "${TEMP_ROOT}/registered-integration" ||
      fail "integration evidence for ${id} is not a registered CTest name: ${name}"
  done <"${TEMP_ROOT}/integration-evidence"
fi

supported_count="$(wc -l <"${TEMP_ROOT}/supported" | tr -d '[:space:]')"
planned_count="$(wc -l <"${TEMP_ROOT}/planned" | tr -d '[:space:]')"
unit_count="$(wc -l <"${TEMP_ROOT}/unit-evidence" | tr -d '[:space:]')"
integration_count="$(wc -l <"${TEMP_ROOT}/integration-evidence" | tr -d '[:space:]')"
manual_count="$(wc -l <"${TEMP_ROOT}/unique-scenarios" | tr -d '[:space:]')"
printf 'Traceability structure valid: %s supported and %s planned requirements.\n' \
  "${supported_count}" "${planned_count}"
printf 'Evidence mappings: %s unit, %s integration, and %s manual scenarios.\n' \
  "${unit_count}" "${integration_count}" "${manual_count}"
if [[ -n "${BUILD_DIR}" ]]; then
  printf 'Automated evidence names match registered CTest tests in %s.\n' "${BUILD_DIR}"
else
  printf 'Automated evidence registration not checked; use --strict --build-dir DIR.\n'
fi
