# Integration harnesses

## SQLite CLI lifecycle template

`cli_lifecycle.sh` is a local end-to-end shell harness for the public `create`, `apply`, `status`,
and `recover` commands. It verifies exact `-- dbdiff: key=value` metadata, visible transaction
boundaries, direct use of `config.migrations`, split declarative source files, target isolation during
`create`, dry-run behavior, application, recovery, and no-op convergence.

Run it directly against a built executable:

```sh
bash tests/integration/cli_lifecycle.sh "$PWD/build/debug/dbdiff"
```

Set `DBDIFF_CLI_KEEP=1` to retain its temporary workspace after a failure. The harness exits 77 only
when the external `sqlite3` prerequisite is missing. CTest registers it with the labels
`integration`, `integration.sqlite`, and `cli`; an unavailable lifecycle command or command failure
is a test failure, not a skip.

## PostgreSQL lifecycle integration

The PostgreSQL lifecycle case uses one disposable target container and creates independently owned
scratch databases inside it. It checks session statement deadlines, advisory-lock contention and
lock lifetime, canonical identity introspection, fail-closed custom identity options, and scratch
cleanup. It then generates, dry-runs, applies, recovers, and directly inspects two migrations built
from split declarative sources. The schemas exercise PK/UNIQUE/CHECK/FK constraints, an expression
and partial `NULLS NOT DISTINCT` index with `INCLUDE`, and a PUBLIC row-security policy. The second
migration checks dependency ordering and the final no-op creation proves history-to-master
convergence before deliberate live drift is rejected.

## Docker scratch ownership integration

The integration case starts one label-owned PostgreSQL container, waits with `pg_isready`, runs a
query, and explicitly removes the exact container after rechecking all ownership labels. The RAII
destructor repeats cleanup on an unexpected test failure.

The Docker case is built and registered with the `integration.docker` label when
`DBDIFF_BUILD_INTEGRATION_TESTS` is enabled.

Run one PostgreSQL major:

```sh
cmake --preset debug
cmake --build --preset debug
DBDIFF_TEST_POSTGRES_MAJOR=18 bash tests/integration/run_docker_scratch.sh
```

Run both Docker-backed suites for the supported matrix in separate CI jobs:

```sh
for major in 15 16 17 18; do
  DBDIFF_TEST_POSTGRES_MAJOR="$major" \
    ctest --test-dir build/debug --output-on-failure \
      -L '^integration\.(docker|postgresql)$'
done
```

CI first requires `docker info` and pulls the selected image, then runs the CTest labels
`integration.docker` and `integration.postgresql`. It treats any Catch2 `SKIP`/`skipped` result as
a job failure. This is intentional: local developer runs may skip when Docker is unavailable, but
the PostgreSQL 15-18 compatibility matrix is evidence only when every lifecycle case executes.

`DBDIFF_TEST_POSTGRES_IMAGE` selects a custom image while
`DBDIFF_TEST_POSTGRES_MAJOR` continues to declare the expected major. If Docker CLI or its daemon
is unavailable, the runner prints `SKIP` and exits successfully. A missing test binary is an error,
not a skip.

## Requirements evidence

[`../requirements.tsv`](../requirements.tsv) separates shipped requirements from planned work;
[`../evidence.tsv`](../evidence.tsv) maps shipped requirements to exact discovered CTest names and
manual scenario names. Validate structure before configuring, then verify registrations after the
test executables have been built:

```sh
bash tests/scripts/validate_requirements.sh
bash tests/scripts/validate_requirements.sh --strict --build-dir build/debug
```

The strict check enforces each supported row's declared unit/integration evidence contract, rejects
evidence attached to planned requirements, and rejects any automated name that is absent from the
requested build's CTest registry.
