#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
SCENARIO_DIR="${SCRIPT_DIR}/scenarios"
KEEP=0
LIST=0
GROUP=""
declare -a REQUESTED=()
SESSION_ROOT=""

usage() {
  cat <<'EOF'
Usage:
  manual-tests/run.sh --list
  manual-tests/run.sh [--keep] --group ci-safe|docker-smoke|full
  manual-tests/run.sh [--keep] SCENARIO [SCENARIO ...]

No scenarios run without an explicit group or scenario name. Docker scenarios also require:

  DBDIFF_MANUAL_ALLOW_DOCKER=I_UNDERSTAND

Set DBDIFF_BIN to select a non-default dbdiff executable.
EOF
}

scenario_files() {
  find "${SCENARIO_DIR}" -maxdepth 1 -type f -name '[0-9][0-9][0-9]_*.sh' -print | sort
}

list_scenarios() {
  local scenario name description groups
  printf '%-38s %-20s %s\n' "SCENARIO" "GROUPS" "DESCRIPTION"
  while IFS= read -r scenario; do
    name="$(basename -- "${scenario}" .sh)"
    description="$(bash "${scenario}" --describe)"
    groups="$(bash "${scenario}" --groups)"
    printf '%-38s %-20s %s\n' "${name}" "${groups}" "${description}"
  done < <(scenario_files)
}

safe_remove_session() {
  local target="$1"
  local base parent name
  [[ -n "${target}" && -d "${target}" && ! -L "${target}" ]] || return 0
  base="$(CDPATH= cd -- "${TMPDIR:-/tmp}" && pwd -P)"
  parent="$(CDPATH= cd -- "$(dirname -- "${target}")" && pwd -P)"
  name="$(basename -- "${target}")"
  if [[ "${parent}" != "${base}" || "${name}" != dbdiff-manual-session.* ]]; then
    printf 'Refusing to remove unexpected session path: %s\n' "${target}" >&2
    return 1
  fi
  chmod -R u+w -- "${target}" 2>/dev/null || true
  rm -rf -- "${target}"
}

cleanup() {
  local status="${1:-$?}"
  trap - EXIT INT TERM
  if [[ "${KEEP}" == "1" ]]; then
    printf 'Session artifacts retained at %s\n' "${SESSION_ROOT}"
  elif [[ -n "${SESSION_ROOT}" ]]; then
    safe_remove_session "${SESSION_ROOT}"
  fi
  return "${status}"
}

handle_signal() {
  local status="$1"
  cleanup "${status}" || true
  exit "${status}"
}

resolve_scenario() {
  local requested="$1"
  [[ "${requested}" =~ ^[0-9]{3}_[a-z0-9_]+$ ]] || {
    printf 'Invalid scenario name: %s\n' "${requested}" >&2
    return 1
  }
  local path="${SCENARIO_DIR}/${requested}.sh"
  [[ -f "${path}" && ! -L "${path}" ]] || {
    printf 'Unknown scenario: %s\n' "${requested}" >&2
    return 1
  }
  printf '%s\n' "${path}"
}

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --help|-h)
      usage
      exit 0
      ;;
    --list)
      LIST=1
      shift
      ;;
    --keep)
      KEEP=1
      shift
      ;;
    --group)
      [[ "$#" -ge 2 ]] || { printf '%s\n' '--group requires a value' >&2; exit 2; }
      GROUP="$2"
      shift 2
      ;;
    --*)
      printf 'Unknown option: %s\n' "$1" >&2
      exit 2
      ;;
    *)
      REQUESTED+=("$1")
      shift
      ;;
  esac
done

if [[ "${LIST}" == "1" ]]; then
  [[ -z "${GROUP}" && "${#REQUESTED[@]}" -eq 0 ]] || {
    printf '%s\n' '--list cannot be combined with scenarios or --group' >&2
    exit 2
  }
  list_scenarios
  exit 0
fi

if [[ -n "${GROUP}" && "${#REQUESTED[@]}" -gt 0 ]]; then
  printf '%s\n' '--group cannot be combined with explicit scenarios' >&2
  exit 2
fi
if [[ -n "${GROUP}" && ! "${GROUP}" =~ ^(ci-safe|docker-smoke|full)$ ]]; then
  printf 'Unknown group: %s\n' "${GROUP}" >&2
  exit 2
fi
if [[ -z "${GROUP}" && "${#REQUESTED[@]}" -eq 0 ]]; then
  usage
  printf '\n'
  list_scenarios
  exit 0
fi

if [[ -n "${GROUP}" ]]; then
  while IFS= read -r scenario; do
    if [[ " $(bash "${scenario}" --groups) " == *" ${GROUP} "* ]]; then
      REQUESTED+=("$(basename -- "${scenario}" .sh)")
    fi
  done < <(scenario_files)
fi

[[ "${#REQUESTED[@]}" -gt 0 ]] || {
  printf 'No scenarios belong to group %s\n' "${GROUP}" >&2
  exit 2
}

require_tmp="${TMPDIR:-/tmp}"
[[ -d "${require_tmp}" && ! -L "${require_tmp}" ]] || {
  printf 'Unsafe temporary directory: %s\n' "${require_tmp}" >&2
  exit 1
}
SESSION_ROOT="$(mktemp -d "${require_tmp}/dbdiff-manual-session.XXXXXX")"
SESSION_ROOT="$(CDPATH= cd -- "${SESSION_ROOT}" && pwd -P)"
trap cleanup EXIT
trap 'handle_signal 130' INT
trap 'handle_signal 143' TERM
export DBDIFF_MANUAL_SESSION_ROOT="${SESSION_ROOT}"
export DBDIFF_MANUAL_KEEP="${KEEP}"

failures=0
for requested in "${REQUESTED[@]}"; do
  scenario="$(resolve_scenario "${requested}")"
  printf '\n=== %s ===\n' "${requested}"
  if ! bash "${scenario}"; then
    printf 'Scenario failed: %s\n' "${requested}" >&2
    failures=$((failures + 1))
  fi
done

if [[ "${failures}" -ne 0 ]]; then
  printf '\n%d scenario(s) failed.\n' "${failures}" >&2
  exit 1
fi
printf '\nAll %d scenario(s) passed.\n' "${#REQUESTED[@]}"
