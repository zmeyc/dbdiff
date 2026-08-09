# dbdiff

`dbdiff` manages database schema changes from two version-controlled inputs:

- an ordered directory of SQL migrations, which defines the last released state; and
- one or more declarative SQL files, which define the desired master schema.

Migration creation never compares against the configured live database. It reconstructs the full
migration history in a fresh empty database, constructs the master schema independently in another
fresh empty database, and plans the change between those two states. The live target is used only by
`apply`, `status`, and `recover`.

SQLite and PostgreSQL are available as backends. Read [Backend scope](#backend-scope) before using
the PostgreSQL planner; its currently managed object set is deliberately focused.

## Quick start with SQLite

Create this layout:

```text
example/
├── dbdiff.yaml
├── migrations/
└── schema/
    ├── dbdiff.schema
    ├── utils.sql
    └── auth.sql
```

`dbdiff.yaml`:

```yaml
format: 1
backend: sqlite
database: sqlite:app.sqlite
sources:
  - schema
migrations: migrations
```

`schema/dbdiff.schema` controls the order of the declarative files:

```text
utils.sql
auth.sql
```

For example, `schema/auth.sql` can contain:

```sql
CREATE TABLE users (
  id INTEGER PRIMARY KEY,
  email TEXT NOT NULL UNIQUE
) STRICT;
```

Create and inspect the first migration, then apply it:

```sh
dbdiff create --name initial --allow-hazard WRITE_LOCK
sed -n '1,120p' migrations/*.sql
dbdiff status                 # exits 2 while work is pending
dbdiff apply --create-database
dbdiff status                 # exits 0 when converged
```

The migration is ordinary SQL with metadata comments and visible `BEGIN`/`COMMIT` statements. It is
intended to be reviewed and may be edited before application.

## How migration creation works

For every `create`, dbdiff performs this sequence:

1. Load and validate every migration from `config.migrations` in lexical filename order.
2. Create a fresh empty history database and replay the entire migration chain into it.
3. Create a second fresh empty database and execute all declarative sources into it in resolved
   source order.
4. Compare normalized semantic snapshots of the reconstructed history and master databases.
5. Plan SQL, require explicit approval for every reported hazard, apply the candidate to the fresh
   history database, and require it to converge exactly to master.
6. Atomically save one migration directly in `config.migrations`, or report that the schema is
   unchanged.

The configured target locator is not resolved during this process. A missing, unreachable, or
intentionally invalid live target therefore cannot influence generated SQL. PostgreSQL creation
does need a scratch provisioning mechanism; see [PostgreSQL scratch databases](#postgresql-scratch-databases).

## Configuration

dbdiff reads strict YAML format 1. Pass a file with `--config`, or let dbdiff search for
`dbdiff.yaml` from the working directory upward. Discovery stops at the nearest Git root and never
uses a config above it. Relative paths are resolved from the configuration file's directory.

Unknown keys, YAML aliases, anchors, custom tags, merge keys, and multiple YAML documents are
rejected.

### SQLite

```yaml
format: 1
backend: sqlite
database_env: APP_DATABASE_URL
sources:
  - schema
migrations: migrations
lock_timeout: 5s
statement_timeout: 5m
```

```sh
export APP_DATABASE_URL='sqlite:var/app.sqlite'
```

A SQLite locator must be `sqlite:` followed by a plain persistent filesystem path. Relative target
paths are resolved from `dbdiff.yaml`. URI locators, query strings, `:memory:`, symlinks, and
non-regular target files are rejected. `apply --create-database` is required before dbdiff will
create a missing SQLite target; `create` and `apply --dry-run` never create it.

### PostgreSQL

```yaml
format: 1
backend: postgresql
database_env: APP_DATABASE_URL
sources:
  - schema
migrations: migrations
scratch:
  database_env: DBDIFF_SCRATCH_URL
managed_schemas:
  - public
  - app
lock_timeout: 5s
statement_timeout: 5m
```

PostgreSQL locators may use a URI or libpq keyword/value syntax. Environment-backed locators are
recommended so credentials do not enter version control:

```sh
export APP_DATABASE_URL='postgresql://app:secret@db.example/app'
export DBDIFF_SCRATCH_URL='postgresql://dbdiff:secret@127.0.0.1/postgres'
```

If `managed_schemas` is omitted, it defaults to `public`. Only explicitly managed schemas
participate in PostgreSQL semantic snapshots. PostgreSQL 15, 16, 17, and 18 are accepted, and all
migrations in a chain must target the same major version.

### Configuration fields

| Field | Meaning |
| --- | --- |
| `format` | Required configuration format; currently `1`. |
| `backend` | Required: `sqlite` or `postgresql`. |
| `database` | Literal live-target locator. Mutually exclusive with `database_env`. |
| `database_env` | Environment variable containing the live-target locator. |
| `sources` | Required non-empty ordered list of SQL files, directories, manifests, or `-` for stdin. |
| `migrations` | Required migration directory. Generated files are stored directly here, without a backend subdirectory. |
| `scratch.database` | PostgreSQL provisioning locator used to create fresh scratch databases. |
| `scratch.database_env` | Environment variable containing the PostgreSQL provisioning locator. |
| `scratch.docker.image` | Run an ephemeral PostgreSQL image instead of using an existing provisioning server. |
| `managed_schemas` | PostgreSQL schemas included in semantic snapshots; defaults to `[public]`. |
| `lock_timeout` | Positive duration using `ms`, `s`, `m`, or `h`; default `5s`. |
| `statement_timeout` | Positive duration using `ms`, `s`, `m`, or `h`; default `5m`. |

Timeout fields are parsed and validated, but enforcement is not yet uniform across every backend
operation.

### Splitting the declarative schema

Each `sources` entry can be:

- a `.sql` file;
- a directory, recursively resolved in normalized lexical order;
- a `dbdiff.schema` or `*.dbdiff-schema` manifest; or
- `-`, meaning standard input.

When a directory contains `dbdiff.schema`, the manifest determines its order instead of recursive
lexical discovery. A manifest has one relative entry per line, supports `#` comments, and can refer
to SQL files, directories, or nested manifests:

```text
# schema/dbdiff.schema
utils.sql
auth/auth.dbdiff-schema
reporting
```

Absolute paths, `..`, globs, cycles, duplicates, symlinks, invalid UTF-8, and overlap with the
migration directory are rejected. Reorganizing files can change the exact source-set hash while
leaving the semantic schema unchanged; in that case `create` emits no migration.

Declarative inputs are schema definitions, not seed scripts. DML, transaction control, temporary
objects, server-wide operations, client commands, and writes to dbdiff's metadata objects are not
allowed. Put intentional data changes inside an explicit transaction in a reviewed migration.

### PostgreSQL scratch databases

PostgreSQL needs two fresh databases for `create`: one for reconstructed history and one for master.
Choose exactly one provisioning mode:

- `scratch.database` or `scratch.database_env`: connect to an existing maintenance database. The
  role must be able to create and drop databases. Each scratch database is created from `template0`,
  marked with a unique ownership token, and removed only after ownership checks pass.
- `scratch.docker.image`: start an ephemeral, label-owned container through the Docker CLI, then
  create the two scratch databases inside it. The container is explicitly removed after use.

Example Docker configuration:

```yaml
scratch:
  docker:
    image: postgres:18
```

Docker is never selected implicitly. An existing Docker CLI and reachable daemon are required for
this mode.

## Command-line interface

Global options precede the command in the examples:

```text
dbdiff [--config PATH] <create|apply|status|recover> ...
dbdiff --help
dbdiff --version
```

### `create`

```sh
dbdiff --config dbdiff.yaml create --name add_sessions \
  --allow-hazard WRITE_LOCK \
  --allow-hazard TABLE_REWRITE
```

`--name` is required. It is normalized to a lowercase ASCII slug and combined with a UTC timestamp
to form `YYYYMMDDHHMMSS_slug.sql`. If the current second would not sort after the last migration,
dbdiff advances the generated timestamp to preserve strict lexical order. Repeated
`--allow-hazard` options approve exactly the hazards named in the generated file. Missing approvals
stop creation before a migration is saved.

Supported hazard names are:

```text
DATA_LOSS
DATA_MIGRATION_REQUIRED
TABLE_REWRITE
WRITE_LOCK
CONSTRAINT_SCAN
NONTRANSACTIONAL
ENUM_REBUILD
MATERIALIZED_VIEW_REBUILD
BREAKING_ROUTINE_CHANGE
UNTRACKABLE_ROUTINE_DEPENDENCY
ROWID_REASSIGNMENT
```

The planners do not guess ambiguous renames, data mappings, or unsafe conversions. Such changes fail
closed or produce draft diagnostics requiring manual SQL. Draft migrations are not eligible for
normal reconstruction or application until the ambiguity is resolved and the candidate converges.

### `apply`

```sh
dbdiff apply
dbdiff apply --dry-run
dbdiff apply --create-database       # SQLite only
dbdiff apply --validate-data         # SQLite data-bearing validation
dbdiff apply --resume                # explicit continuation after a partial failure
```

Before writing, `apply` validates the on-disk chain and stored history, reconstructs the applied
prefix in a fresh database, and compares that result with the live schema. Live drift blocks the
operation; it is never turned into repair SQL. Pending migrations are then applied in lexical order
using the transaction boundaries visible in each SQL file.

`--dry-run` performs validation without changing schema or history. `--create-database` permits
creation of an absent SQLite file and has no PostgreSQL equivalent. `--validate-data` uses a copy of
the SQLite target to test pending work against real data before live writes. `--resume` is required
when the last attempt is incomplete; it is rejected when there is nothing incomplete to resume.

### `status`

```sh
dbdiff status
```

`status` performs the same history-prefix and live-drift checks without applying anything. It exits
0 only when the target is converged. Pending migrations, drift, or a missing target are reported as
action-required status 2.

### `recover`

Every attempted SQL revision is stored in the target database before execution. Recover the latest
stored bytes for a migration version:

```sh
dbdiff recover 20260808123000_add_sessions > recovered.sql
```

List all stored revisions and their hashes:

```sh
dbdiff recover 20260808123000_add_sessions --list
```

List output is tab-separated: migration version, zero-based revision ordinal, and exact SHA-256.
`recover` reads database history and does not need to write the migration directory.

### Exit behavior

- `0`: command succeeded; for `status`, the target is converged.
- `1`: dbdiff configuration, validation, database, or execution failure.
- `2`: `status` completed successfully but found action required.
- Other nonzero values may be returned by CLI argument parsing errors.

## Migration files

Migrations are UTF-8 files named `YYYYMMDDHHMMSS_slug.sql` and loaded in lexical order. Symlinks are
not accepted. Each file begins with strict metadata using the exact `-- dbdiff: key=value` form:

```sql
-- dbdiff: format=1
-- dbdiff: backend=sqlite
-- dbdiff: version=20260808123000_add_sessions
-- dbdiff: engine-version=3.45.3
-- dbdiff: from-sha256=<64 lowercase hex characters>
-- dbdiff: to-sha256=<64 lowercase hex characters>
-- dbdiff: source-set-sha256=<64 lowercase hex characters>
-- dbdiff: allow=WRITE_LOCK
-- dbdiff: draft=false

BEGIN IMMEDIATE;
CREATE TABLE sessions (...);
COMMIT;
```

PostgreSQL migrations use visible `BEGIN;` and `COMMIT;`. SQLite generation normally uses
`BEGIN IMMEDIATE;` and `COMMIT;`, with foreign-key directives around table rebuilds when needed.
These boundaries are deliberately part of the file: reviewers may move or split them, subject to
the resumable-script rules below. dbdiff does not wrap the complete file in a hidden transaction.

Migration DML must be inside an explicit transaction. Standalone DDL can form its own resumable
execution unit. Nested or unterminated transactions, full rollback, ambiguous transaction control,
and unsupported session changes are rejected. PostgreSQL dollar-quoted routine bodies and nested
comments, and SQLite multi-statement trigger bodies, are parsed as single statements.

Editing is allowed before application as long as the SQL still starts from the declared semantic
hash and converges to the declared destination hash. Once a unit has completed, its exact bytes,
order, and transaction shape form an immutable prefix. A failed suffix may be edited and retried
with `apply --resume`; a completed migration must continue matching its stored file checksum.

## Migration history and drift

dbdiff owns internal metadata objects in each target:

- SQLite: `_dbdiff_migrations`, `_dbdiff_migration_revisions`, and
  `_dbdiff_migration_units` in `main`.
- PostgreSQL: `_dbdiff.migrations`, `_dbdiff.revisions`, and `_dbdiff.units`.

Do not create, alter, or write these objects yourself. They record the backend and engine version,
attempted and completed file hashes, exact attempted SQL, execution-unit hashes, visible transaction
shape, before/after semantic checkpoints, and started/completed state.

An explicit transaction is checkpointed with its commit. A standalone unit is recorded before
execution and reconciled against its before/after semantic checkpoints after interruption. Resume
continues only when the database and stored prefix make the outcome unambiguous; otherwise dbdiff
fails and asks for manual reconciliation. `recover` remains available for retrieving every stored
attempt.

Drift is always evaluated against the state reconstructed from the applied migration prefix, not
against the current master schema. This distinguishes unauthorized live edits from legitimate
pending migrations.

## Backend scope

### SQLite

The SQLite backend semantically tracks ordinary tables, columns, declared types, defaults,
generated columns, primary/unique/check/foreign-key constraints, STRICT, WITHOUT ROWID, and
AUTOINCREMENT properties, indexes (including expression and partial indexes), views, and triggers.
Changes use native ALTER when it is safe and deterministic; other supported table changes use a
transactional rebuild that preserves mapped rows, accessible rowids, `sqlite_sequence`, foreign
keys, indexes, views, and triggers.

Temporary, attached, virtual, shadow, and reserved metadata objects are outside the managed scope.
Changes needing an unknown data mapping or an unverifiable rowid transition fail closed.

### PostgreSQL

PostgreSQL 15–18 connection, scratch creation, statement parsing, resumable history, and focused
schema planning are implemented. The current semantic snapshot and planner manage only:

- explicitly managed schemas;
- ordinary, non-partitioned, non-extension-owned permanent and UNLOGGED tables;
- table persistence and row-security/force-row-security flags; and
- column order, name, formatted type, nullability, default expression, identity generation,
  generated storage, and explicit collation.

The planner can create and drop managed schemas and tables, append/drop supported columns, change
defaults and nullability, change table persistence, and toggle table RLS flags. It deliberately
does not infer renames, reorder columns, or synthesize unsafe type, collation, identity, or generated
expression conversions.

Constraints, indexes, sequences, enums, domains, views, materialized views, routines, triggers,
policies, comments, partitioned/foreign tables, extension-owned objects, ownership, grants, and
other security properties are not yet semantically managed. The declarative source executor may
accept some persistent PostgreSQL DDL outside the focused set, but that does not make those objects
part of diff generation or drift detection. Do not rely on dbdiff to manage them in this version.

## Building

The baseline is Ubuntu 24.04, C++20, and the CMake version shipped by that distribution; the project
itself requires CMake 3.24 or newer. The checked-in presets use Ninja.

Install the build and test dependencies from Ubuntu packages:

```sh
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config \
  libpqxx-dev libpq-dev libsqlite3-dev sqlite3 \
  libcli11-dev libyaml-cpp-dev libssl-dev catch2
```

Configure, build, and test:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Create a release build and install it under a chosen prefix:

```sh
cmake --preset release
cmake --build --preset release
cmake --install build/release --prefix "$PWD/dist"
```

`DBDIFF_BUILD_INTEGRATION_TESTS=ON` is the default. Docker-backed tests skip when the Docker CLI or
daemon is genuinely unavailable. The local CLI lifecycle test skips only when the external
`sqlite3` executable is missing; command or behavioral failures remain test failures.

### Formatting, clang-tidy, and sanitizers

Install the optional analysis tools:

```sh
sudo apt-get install -y clang-format clang-tidy
```

Check or apply formatting when the CMake targets are available:

```sh
cmake --build --preset debug --target format-check
cmake --build --preset debug --target format
```

Run a compiler build with clang-tidy enabled:

```sh
cmake -S . -B build/tidy -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DDBDIFF_ENABLE_CLANG_TIDY=ON
cmake --build build/tidy --parallel
```

Run AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
cmake --preset asan-ubsan
cmake --build --preset asan-ubsan
ctest --preset asan-ubsan
```

## Automated and manual testing

Run all configured tests with `ctest --preset debug`. Useful focused commands include:

```sh
ctest --test-dir build/debug --output-on-failure -L integration.sqlite
DBDIFF_TEST_POSTGRES_MAJOR=18 \
  bash tests/integration/run_docker_scratch.sh
bash tests/scripts/validate_requirements.sh
```

The requirements validator reports actual Catch2 evidence tags separately from planned targets.
`--strict` lists every missing unit or integration tag and exits nonzero while gaps remain; it is a
release gate, not a way to claim unwritten coverage.

The operator-oriented scenarios in [`manual-tests/README.md`](manual-tests/README.md) are
self-checking and use isolated temporary workspaces. Local SQLite scenarios require `sqlite3`:

```sh
DBDIFF_BIN="$PWD/build/debug/dbdiff" bash manual-tests/run.sh --group ci-safe
```

PostgreSQL scenarios create only disposable, ownership-labeled Docker resources and require an
explicit opt-in:

```sh
export DBDIFF_MANUAL_ALLOW_DOCKER=I_UNDERSTAND
DBDIFF_BIN="$PWD/build/debug/dbdiff" bash manual-tests/run.sh --group docker-smoke
```

Use `bash manual-tests/run.sh --list` to inspect scenarios without side effects. See the manual-test
guide for cleanup guarantees, retained-workspace debugging, PostgreSQL major selection, and the
full requirements matrix workflow.
