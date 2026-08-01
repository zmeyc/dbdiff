# dbdiff

`dbdiff` reconstructs database state from ordered SQL migrations, constructs a separate master
schema from declarative SQL sources, and generates the SQL needed to converge the reconstructed
state to the master schema.

The initial release targets PostgreSQL and SQLite.

## Build

The supported baseline is Ubuntu 24.04 with C++20 and CMake 3.24 or newer.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

