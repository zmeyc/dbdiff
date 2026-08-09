#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
source "${SCRIPT_DIR}/../lib/common.sh"

case "${1:-}" in
  --describe) printf '%s\n' 'SQLite ordered nested manifests, semantic file moves, and invalid paths'; exit 0 ;;
  --groups) printf '%s\n' 'ci-safe full'; exit 0 ;;
esac

manual_init "002_sqlite_multifile_manifest"
require_command sqlite3
require_dbdiff_command create
require_dbdiff_command apply

project="${DBDIFF_MANUAL_RUN_ROOT}/project"
database="${DBDIFF_MANUAL_RUN_ROOT}/target.sqlite"
copy_fixture "sqlite/basic" "${project}"
mkdir -p "${project}/migrations"
write_sqlite_config "${project}/dbdiff.yaml"
export DBDIFF_MANUAL_DATABASE_URL="sqlite:${database}"

manual_step "WHEN resolving a root manifest with a nested manifest"
run_ok "create from nested manifests" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" create --name modular \
  --allow-hazard WRITE_LOCK
run_ok "apply modular schema" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" apply --create-database
assert_sqlite_scalar "${database}" \
  "SELECT count(*) FROM sqlite_schema WHERE type IN ('table','view','trigger') AND name NOT LIKE '_dbdiff%';" \
  "6"

manual_step "THEN moving a source without changing SQL is a semantic no-op"
mkdir -p "${project}/schema/reporting"
mv "${project}/schema/auth/30_reporting.sql" "${project}/schema/reporting/30_reporting.sql"
cat >"${project}/schema/auth/auth.dbdiff-schema" <<'MANIFEST'
10_users.sql
20_posts.sql
MANIFEST
cat >"${project}/schema/dbdiff.schema" <<'MANIFEST'
00_meta.sql
auth/auth.dbdiff-schema
reporting
MANIFEST
before_count="$(migration_count "${project}/migrations")"
run_ok "create after a semantic-only source move" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" create --name moved_sources
assert_equals "${before_count}" "$(migration_count "${project}/migrations")" \
  "migration count after source move"

manual_step "THEN manifest traversal is refused"
cp "${project}/schema/dbdiff.schema" "${DBDIFF_MANUAL_RUN_ROOT}/valid.schema"
cat >"${project}/schema/dbdiff.schema" <<'MANIFEST'
../outside.sql
MANIFEST
run_fails "create with parent traversal" \
  "${DBDIFF_BIN}" --config "${project}/dbdiff.yaml" create --name invalid_manifest
cp "${DBDIFF_MANUAL_RUN_ROOT}/valid.schema" "${project}/schema/dbdiff.schema"

manual_pass
