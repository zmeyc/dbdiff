# Manual testing

These scenarios exercise `dbdiff` as an operator would use it. They supplement, but never replace,
the automated unit and integration suite. Every scenario is self-checking and exits nonzero when an
expectation is not met.

The scripts never edit fixtures in this repository. Each run copies its inputs into a directory made
with `mktemp`, uses a unique database name, and removes only its exact run directory. Docker
containers created directly by the suite carry all of these labels:

```text
io.dbdiff.manual=true
io.dbdiff.manual.run=<unique-run-id>
io.dbdiff.manual.role=<scenario-role>
```

Cleanup verifies the full container ID and ownership labels before removal. It never removes by a
name prefix, wildcard, or broad Docker filter.

## Prerequisites

- A built `dbdiff` executable.
- Bash 3.2 or newer and standard command-line tools.
- `sqlite3` for the local SQLite scenarios.
- Docker CLI and a reachable Docker daemon for PostgreSQL scenarios.
- `shellcheck` only for linting the scripts.

The default executable is `build/debug/dbdiff`. Select another build explicitly:

```sh
export DBDIFF_BIN="$PWD/build/debug/dbdiff"
```

## Discover and run scenarios

Listing scenarios has no side effects:

```sh
bash manual-tests/run.sh --list
```

No arguments also prints help and the scenario list; it does not run anything. Run one scenario or
the local, Docker, or complete disposable groups explicitly:

```sh
bash manual-tests/run.sh 001_sqlite_initial
bash manual-tests/run.sh --group ci-safe

export DBDIFF_MANUAL_ALLOW_DOCKER=I_UNDERSTAND
bash manual-tests/run.sh --group docker-smoke
bash manual-tests/run.sh --group full
```

`full` includes the local and disposable-Docker scenarios. It does not target an existing external
database server. Docker scenarios refuse to start unless the opt-in variable has the exact value
shown above.

Use `--keep` to retain the temporary workspace and exact Docker containers for investigation:

```sh
bash manual-tests/run.sh --keep 005_sqlite_resume_recover
```

The runner prints retained paths and container IDs, but never passwords. You are responsible for
removing retained artifacts. Before removing a retained container, inspect its three ownership
labels and use the exact container ID.

## Scenario catalog

| Scenario | Expected checkpoints |
| --- | --- |
| `001_sqlite_initial` | `create` does not create/open the target; generated SQL has visible transaction control; dry-run is inert; apply converges; direct SQLite replay succeeds; a second create is empty. |
| `002_sqlite_multifile_manifest` | Ordered and nested schema manifests load; reorganizing semantically identical files produces no migration; invalid manifest traversal is refused. |
| `003_sqlite_rebuild_preserves_data` | A table rebuild preserves rows, row IDs, AUTOINCREMENT state, foreign keys, generated values, views, indexes, and triggers. |
| `004_sqlite_drift_isolation` | Manual live DDL makes status return 2 and blocks apply, while create remains based only on history and master and does not modify the target. |
| `005_sqlite_resume_recover` | A committed unit survives a later failure; edits to that unit are rejected; an edited incomplete suffix resumes; stored SQL revisions are recoverable. |
| `010_postgresql_initial` | Split PostgreSQL sources create constraints, an advanced index, and a row-security policy without resolving the target; dry-run is inert; apply and exact recovery converge; direct `psql` replay succeeds; a no-op create remains target-independent; live drift is reported and blocks apply. |

Each scenario logs its GIVEN/WHEN/THEN checkpoints. Command output is captured beneath the scenario
workspace and printed only on failure after common credential patterns have been redacted.

## Failure interpretation

- `dbdiff command is not available yet` means the selected binary predates that workflow.
- Exit status 2 from `status` is expected only where a scenario deliberately creates drift or pending
  work.
- A Docker readiness failure retains nothing unless `--keep` was selected; inspect the redacted log
  shown by the scenario.
- With `--keep`, rerun the exact failing command from the retained workspace after exporting the
  same locator environment variables. Locators are intentionally not written to fixture files.

## Script and traceability checks

Run syntax and static checks without starting a database:

```sh
while IFS= read -r -d '' script; do bash -n "$script"; done \
  < <(find manual-tests tests/scripts tests/integration -type f -name '*.sh' -print0)
shellcheck manual-tests/run.sh manual-tests/lib/*.sh manual-tests/scenarios/*.sh \
  tests/scripts/*.sh tests/integration/*.sh
bash tests/scripts/validate_requirements.sh
```

The requirements matrix is [`tests/requirements.tsv`](../tests/requirements.tsv), and
[`tests/evidence.tsv`](../tests/evidence.tsv) maps supported requirements to exact CTest or manual
scenario names. Each supported row declares whether unit evidence, integration evidence, or both
are required; planned requirements must declare no required evidence and have no evidence rows.
The structural check is useful before a build; it deliberately does not claim that automated
evidence is registered:

```sh
bash tests/scripts/validate_requirements.sh
```

After building, the strict release gate verifies every automated evidence name against CTest's
actual unit and integration registrations. It gates supported scope only:

```sh
bash tests/scripts/validate_requirements.sh --strict --build-dir build/debug
```

## Suggested CI execution

```sh
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DDBDIFF_BUILD_INTEGRATION_TESTS=ON
cmake --build build/debug --parallel
ctest --test-dir build/debug --output-on-failure -L unit
ctest --test-dir build/debug --output-on-failure -L integration.sqlite
bash tests/scripts/validate_requirements.sh --strict --build-dir build/debug
DBDIFF_BIN="$PWD/build/debug/dbdiff" bash manual-tests/run.sh --group ci-safe

export DBDIFF_MANUAL_ALLOW_DOCKER=I_UNDERSTAND
DBDIFF_BIN="$PWD/build/debug/dbdiff" bash manual-tests/run.sh --group docker-smoke
```

Run PostgreSQL integration tests independently for images 15, 16, 17, and 18 so a version-specific
failure is attributable to one job.
