#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(CDPATH= cd -- "${SCRIPT_DIR}/../.." && pwd -P)"
TEST_BINARY="${DBDIFF_DOCKER_INTEGRATION_BIN:-${REPO_ROOT}/build/debug/tests/integration/dbdiff_docker_integration_tests}"

if ! command -v docker >/dev/null 2>&1; then
  printf '%s\n' 'SKIP: Docker CLI is unavailable'
  exit 0
fi
if command -v timeout >/dev/null 2>&1; then
  docker_available() { timeout 10s docker info >/dev/null 2>&1; }
else
  docker_available() { docker info >/dev/null 2>&1; }
fi
if ! docker_available; then
  printf '%s\n' 'SKIP: Docker daemon is unavailable'
  exit 0
fi
if [[ ! -x "${TEST_BINARY}" ]]; then
  printf 'Integration binary is missing or not executable: %s\n' "${TEST_BINARY}" >&2
  printf '%s\n' 'Configure with DBDIFF_BUILD_INTEGRATION_TESTS=ON and build first.' >&2
  exit 2
fi

if [[ -n "${DBDIFF_TEST_POSTGRES_MAJOR:-}" ]]; then
  case "${DBDIFF_TEST_POSTGRES_MAJOR}" in
    15|16|17|18) ;;
    *) printf '%s\n' 'DBDIFF_TEST_POSTGRES_MAJOR must be 15, 16, 17, or 18' >&2; exit 2 ;;
  esac
fi

exec "${TEST_BINARY}" '[integration][docker]' --reporter compact
