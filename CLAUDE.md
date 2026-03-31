# duckdb — Handoff Notes

## dev.branch

| Item | Value |
|------|-------|
| Active branch | `hbt/v1.5.1` |
| Base tag | `v1.5.1` (upstream Variegata) |
| Feature commit | `d2e1da955d` — `[lock] Add LOCK_CONFIG 'try' for read-only connections` |
| Installed binary | `/usr/local/bin/duckdb` |
| Backup (stock) | `/usr/local/bin/duckdb.v1.5.1-stock.bak` |

---

## dev.build

### dev.build.release

```bash
cd /home/hassen/workspace/duckdb

make release BUILD_EXTENSIONS="json;icu;autocomplete;jemalloc;parquet;core_functions;fts;vss"
```

> **Build time:** ~45 minutes on this machine. Always run in background; wait for completion before verifying.

- Output binary: `build/release/duckdb`
- `fts` and `vss` are fetched via `extension/extension_config_local.cmake` (GIT_TAG `v1.5.1`)
- `jemalloc` is auto-included on 64-bit Linux — no explicit flag needed
- `parquet` and `core_functions` are always built-in
- `shell` is always linked into the CLI binary

### dev.build.debug

```bash
make debug BUILD_EXTENSIONS="json;icu;autocomplete;jemalloc;parquet;core_functions;fts;vss"
# Output: build/debug/duckdb
```

### dev.build.install

```bash
cd build/release && sudo cmake --install .
# Installs to /usr/local/bin/duckdb
```

Backup first:

```bash
sudo cp /usr/local/bin/duckdb /usr/local/bin/duckdb.$(date +%Y%m%d_%H%M%S).bak
```

Rollback:

```bash
sudo cp /usr/local/bin/duckdb.v1.5.1-stock.bak /usr/local/bin/duckdb
```

---

## dev.extensions

### dev.extensions.local-config

File: `extension/extension_config_local.cmake`

```cmake
duckdb_extension_load(fts
    GIT_URL https://github.com/duckdb/duckdb-fts
    GIT_TAG v1.5.1
)
duckdb_extension_load(vss
    GIT_URL https://github.com/duckdb/duckdb-vss
    GIT_TAG v1.5.1
)
```

This file is **not tracked upstream** — it pins fts/vss to `v1.5.1`.

### dev.extensions.verify

```bash
duckdb -c "
SELECT extension_name, loaded, installed, install_mode
FROM duckdb_extensions()
WHERE installed
ORDER BY extension_name;"
```

Expected output — all 9 extensions `STATICALLY_LINKED`:

| extension_name | loaded | installed | install_mode |
|----------------|--------|-----------|--------------|
| autocomplete | true | true | STATICALLY_LINKED |
| core_functions | true | true | STATICALLY_LINKED |
| fts | true | true | STATICALLY_LINKED |
| icu | true | true | STATICALLY_LINKED |
| jemalloc | true | true | STATICALLY_LINKED |
| json | true | true | STATICALLY_LINKED |
| parquet | true | true | STATICALLY_LINKED |
| shell | true | true | STATICALLY_LINKED |
| vss | true | true | STATICALLY_LINKED |

---

## dev.tests

### dev.tests.run-all

```bash
cd /home/hassen/workspace/duckdb
make unittest
```

### dev.tests.locking

The `test_locking.cpp` tests use `[.]` (hidden) tags — run them explicitly:

```bash
build/release/test/unittest "[persistence]"
# or a specific test:
build/release/test/unittest "Test LOCK_CONFIG TRY"
```

Test file: `test/persistence/test_locking.cpp`

### dev.tests.single-file

```bash
build/release/test/unittest "test name here"
# or by file tag:
build/release/test/unittest "[tag]"
```

---

## dev.feature.lock-config

### dev.feature.lock-config.what

Adds `LockConfig { DEFAULT, TRY }` to `DBConfigOptions`. All `READ_ONLY` connections **implicitly** use TRY behavior (non-blocking `flock` — on conflict `EAGAIN`/`EACCES` the open proceeds without erroring). Explicit `lock_config='TRY'` is accepted but redundant for READ_ONLY connections.

Use case: open a database file that another process has locked (e.g., a writer), without blocking or crashing. No special option needed — just open READ_ONLY.

### dev.feature.lock-config.sql

```sql
-- READ_ONLY is sufficient — TRY behavior is implicit.
ATTACH '/path/to/file.duckdb' AS db (READ_ONLY);
SELECT * FROM db.some_table LIMIT 10;

-- Explicit form still works (redundant but valid):
ATTACH '/path/to/file.duckdb' AS db (READ_ONLY, lock_config='TRY');
```

Example with the CCE sessions database:

```sql
ATTACH '/home/hassen/workspace/cce/data/claude_sessions.duckdb'
  AS sessions (READ_ONLY);
SELECT * FROM sessions.claude_sessions LIMIT 5;
```

### dev.feature.lock-config.cpp

```cpp
// READ_ONLY is sufficient — TRY is implicit.
DBConfig config;
config.options.access_mode = AccessMode::READ_ONLY;

DuckDB db("/path/to/file.duckdb", &config);
Connection con(db);
```

### dev.feature.lock-config.files

| File | Change |
|------|--------|
| `src/include/duckdb/main/config.hpp` | `LockConfig` enum + `lock_config` field in `DBConfigOptions` |
| `src/include/duckdb/common/file_open_flags.hpp` | `TRY_READ_LOCK` added to `FileLockType` |
| `src/include/duckdb/storage/storage_options.hpp` | `try_lock_on_conflict` bool in `StorageOptions` |
| `src/common/local_file_system.cpp` | Non-blocking `flock`, swallow `EAGAIN`/`EACCES` |
| `src/main/attached_database.{hpp,cpp}` | Parse `lock_config` ATTACH option, propagate to `StorageOptions` |
| `src/storage/single_file_block_manager.{hpp,cpp}` | Forward `try_lock_on_conflict` through `BlockManagerOptions` |
| `src/storage/storage_manager.cpp` | Wire `StorageOptions.try_lock_on_conflict` into block manager |
| `src/common/enum_util.{hpp,cpp}` | `LockConfig` enum serialisation |
| `test/persistence/test_locking.cpp` | Tests for TRY mode conflict behaviour |

### dev.feature.lock-config.constraints

- `TRY` is only valid with `READ_ONLY` access mode — throws `InvalidInputException` otherwise
- When lock is skipped, WAL may not be replayed → reads may be slightly stale vs. an active writer
