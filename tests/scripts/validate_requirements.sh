#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(CDPATH= cd -- "${SCRIPT_DIR}/../.." && pwd -P)"
MATRIX="${REPO_ROOT}/tests/requirements.tsv"
SCENARIO_DIR="${REPO_ROOT}/manual-tests/scenarios"
STRICT_EVIDENCE=0
TEMP_ROOT=""

usage() {
  cat <<'EOF'
Usage: tests/scripts/validate_requirements.sh [--strict|--verify-test-ids]

Without options, validate matrix structure and manual-scenario links, then report
the Catch2 evidence tags that actually exist. With --strict (or the legacy
--verify-test-ids spelling), fail when any planned [unit][REQ-ID] or
[integration][REQ-ID] evidence tag is missing.
EOF
}

fail() {
  printf 'requirements matrix: %s\n' "$*" >&2
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

case "${1:-}" in
  '') ;;
  --strict|--verify-test-ids) STRICT_EVIDENCE=1 ;;
  --help|-h) usage; exit 0 ;;
  *) usage >&2; exit 2 ;;
esac
[[ "$#" -le 1 ]] || { usage >&2; exit 2; }

[[ -f "${MATRIX}" && ! -L "${MATRIX}" ]] || fail "missing regular file ${MATRIX}"
[[ -d "${SCENARIO_DIR}" && ! -L "${SCENARIO_DIR}" ]] || fail "missing scenario directory"
command -v awk >/dev/null 2>&1 || fail "awk is required"
command -v mktemp >/dev/null 2>&1 || fail "mktemp is required"
TEMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/dbdiff-requirements.XXXXXX")"
TEMP_ROOT="$(CDPATH= cd -- "${TEMP_ROOT}" && pwd -P)"
trap safe_cleanup EXIT
trap 'handle_signal 130' INT
trap 'handle_signal 143' TERM

if ! awk -F '\t' 'NF != 6 { print NR ":" NF; bad=1 } END { exit bad }' "${MATRIX}" \
  >"${TEMP_ROOT}/bad-fields"; then
  cat "${TEMP_ROOT}/bad-fields" >&2
  fail "every row must contain exactly six tab-separated fields"
fi

expected_header=$'id\tarea\trequirement\tunit_target\tintegration_target\tmanual_scenario'
IFS= read -r actual_header <"${MATRIX}"
[[ "${actual_header}" == "${expected_header}" ]] || fail "unexpected header"

declare -a requirement_ids=()
: >"${TEMP_ROOT}/ids"
: >"${TEMP_ROOT}/referenced-scenarios"
line_number=1
while IFS=$'\t' read -r id area requirement unit_target integration_target manual_scenario; do
  line_number=$((line_number + 1))
  [[ -n "${id}${area}${requirement}${unit_target}${integration_target}${manual_scenario}" ]] ||
    continue
  [[ "${id}" =~ ^[A-Z][A-Z0-9]*-[0-9]{3}$ ]] || fail "invalid ID at line ${line_number}: ${id}"
  if grep -Fxq -- "${id}" "${TEMP_ROOT}/ids"; then
    fail "duplicate ID at line ${line_number}: ${id}"
  fi
  printf '%s\n' "${id}" >>"${TEMP_ROOT}/ids"
  requirement_ids+=("${id}")
  [[ -n "${area}" && -n "${requirement}" ]] || fail "empty description at line ${line_number}"
  [[ "${unit_target}" == "unit:${id}" ]] || fail "invalid planned unit tag for ${id}"
  [[ "${integration_target}" == "integration:${id}" ]] ||
    fail "invalid planned integration tag for ${id}"
  if [[ "${manual_scenario}" != "-" ]]; then
    [[ "${manual_scenario}" =~ ^[0-9]{3}_[a-z0-9_]+$ ]] ||
      fail "invalid manual scenario for ${id}: ${manual_scenario}"
    [[ -f "${SCENARIO_DIR}/${manual_scenario}.sh" &&
       ! -L "${SCENARIO_DIR}/${manual_scenario}.sh" ]] ||
      fail "missing manual scenario for ${id}: ${manual_scenario}"
    if ! grep -Fxq -- "${manual_scenario}" "${TEMP_ROOT}/referenced-scenarios"; then
      printf '%s\n' "${manual_scenario}" >>"${TEMP_ROOT}/referenced-scenarios"
    fi
  fi
done < <(tail -n +2 "${MATRIX}")

[[ "${#requirement_ids[@]}" -gt 0 ]] || fail "matrix contains no requirements"

while IFS= read -r scenario_file; do
  scenario="$(basename -- "${scenario_file}" .sh)"
  grep -Fxq -- "${scenario}" "${TEMP_ROOT}/referenced-scenarios" ||
    fail "manual scenario is not linked from the matrix: ${scenario}"
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

while IFS= read -r test_source; do
  printf '%s\n' "${test_source}" >>"${TEMP_ROOT}/test-sources"
done < <(find "${REPO_ROOT}/tests" -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' \) \
  -print | sort)
[[ -s "${TEMP_ROOT}/test-sources" ]] || fail "no C++ test sources found"

: >"${TEMP_ROOT}/unit-evidence"
: >"${TEMP_ROOT}/integration-evidence"
: >"${TEMP_ROOT}/missing-unit"
: >"${TEMP_ROOT}/missing-integration"
for id in "${requirement_ids[@]}"; do
  unit_found=0
  integration_found=0
  while IFS= read -r test_source; do
    if grep -Eq "\\[unit\\].*\\[${id}\\]|\\[${id}\\].*\\[unit\\]" "${test_source}"; then
      unit_found=1
    fi
    if grep -Eq "\\[integration\\].*\\[${id}\\]|\\[${id}\\].*\\[integration\\]" \
      "${test_source}"; then
      integration_found=1
    fi
  done <"${TEMP_ROOT}/test-sources"
  if [[ "${unit_found}" == "1" ]]; then
    printf '%s\n' "${id}" >>"${TEMP_ROOT}/unit-evidence"
  else
    printf '%s\n' "${id}" >>"${TEMP_ROOT}/missing-unit"
  fi
  if [[ "${integration_found}" == "1" ]]; then
    printf '%s\n' "${id}" >>"${TEMP_ROOT}/integration-evidence"
  else
    printf '%s\n' "${id}" >>"${TEMP_ROOT}/missing-integration"
  fi
done

manual_count="$(sed '/^$/d' "${TEMP_ROOT}/referenced-scenarios" | wc -l | tr -d '[:space:]')"
unit_count="$(wc -l <"${TEMP_ROOT}/unit-evidence" | tr -d '[:space:]')"
integration_count="$(wc -l <"${TEMP_ROOT}/integration-evidence" | tr -d '[:space:]')"
missing_unit_count="$(wc -l <"${TEMP_ROOT}/missing-unit" | tr -d '[:space:]')"
missing_integration_count="$(wc -l <"${TEMP_ROOT}/missing-integration" | tr -d '[:space:]')"

printf 'Matrix structure valid: %d requirements and %d linked manual scenarios.\n' \
  "${#requirement_ids[@]}" "${manual_count}"
printf 'Catch2 evidence tags found: %d unit and %d integration (of %d planned targets each).\n' \
  "${unit_count}" "${integration_count}" "${#requirement_ids[@]}"

if [[ "${missing_unit_count}" -gt 0 || "${missing_integration_count}" -gt 0 ]]; then
  printf 'Evidence gaps remain: %d unit and %d integration tags are missing.\n' \
    "${missing_unit_count}" "${missing_integration_count}"
fi

if [[ "${STRICT_EVIDENCE}" == "1" ]] &&
  [[ "${missing_unit_count}" -gt 0 || "${missing_integration_count}" -gt 0 ]]; then
  while IFS= read -r id; do
    printf 'missing Catch2 [unit][%s] evidence tag\n' "${id}" >&2
  done <"${TEMP_ROOT}/missing-unit"
  while IFS= read -r id; do
    printf 'missing Catch2 [integration][%s] evidence tag\n' "${id}" >&2
  done <"${TEMP_ROOT}/missing-integration"
  fail "strict evidence check found missing tags"
fi
