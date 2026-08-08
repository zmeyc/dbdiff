# Docker scratch integration test

The integration case starts one label-owned PostgreSQL container, waits with `pg_isready`, runs a
query, and explicitly removes the exact container after rechecking all ownership labels. The RAII
destructor repeats cleanup on an unexpected test failure.

The parent test build must compile `src/core/docker.cpp` into `dbdiff_core`, add
`tests/unit/test_docker.cpp` to the unit target, and include this directory when
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
