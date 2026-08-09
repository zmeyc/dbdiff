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

## Docker scratch integration test

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

Run the supported matrix in separate CI jobs:

```sh
for major in 15 16 17 18; do
  DBDIFF_TEST_POSTGRES_MAJOR="$major" bash tests/integration/run_docker_scratch.sh
done
```

`DBDIFF_TEST_POSTGRES_IMAGE` selects a custom image while
`DBDIFF_TEST_POSTGRES_MAJOR` continues to declare the expected major. If Docker CLI or its daemon
is unavailable, the runner prints `SKIP` and exits successfully. A missing test binary is an error,
not a skip.
