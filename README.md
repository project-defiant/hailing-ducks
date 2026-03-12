# hailing-ducks

A DuckDB extension that reads [Hail](https://hail.is/) native binary file formats
**without a JVM** — no Spark, no Python Hail package required.

The extension registers DuckDB table functions so you can query Hail data directly
in SQL, join it with other tables, and export it to any format DuckDB supports.

All file I/O is routed through DuckDB's virtual filesystem (VFS), so any storage
backend supported by DuckDB — local disk, Amazon S3, Google Cloud Storage, HTTP/HTTPS
— works transparently once the appropriate DuckDB extension (e.g. `httpfs`) is loaded.

## Supported formats

| Hail type | File pattern | DuckDB function |
|-----------|--------------|-----------------|
| BlockMatrix | `<name>.bm/` | `hail_scan_blockmatrix(path)` |
| HailTable *(Phase 2 — planned)* | `<name>.ht/` | `hail_scan_table(path)` |

---

## Prerequisites

| Tool | Minimum version | Notes |
|------|----------------|-------|
| CMake | 3.5 | |
| C++ compiler | C++17 (GCC 9 / Clang 11 / MSVC 19.14) | |
| [DuckDB](https://duckdb.org/) | v1.3.0 | For loading the extension at runtime |
| [lz4](https://lz4.github.io/lz4/) | any | Hail BlockMatrix block compression |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.x | Metadata parsing |
| [OpenSSL](https://www.openssl.org/) | 1.1+ | Required by the DuckDB build system |

### macOS (Homebrew)

```sh
brew install lz4 nlohmann-json openssl cmake ninja ccache
```

### Linux (apt)

```sh
sudo apt install -y liblz4-dev nlohmann-json3-dev libssl-dev cmake ninja-build ccache
```

---

## Building

### 1 — Clone with submodules

```sh
git clone --recurse-submodules https://github.com/<your-org>/hailing-ducks.git
cd hailing-ducks
```

If you already cloned without `--recurse-submodules`:

```sh
git submodule update --init --recursive
```

### 2 — (Optional) Set up vcpkg

vcpkg is an alternative way to supply `lz4`, `nlohmann-json`, and `openssl`
on platforms where system packages are unavailable.  Skip this step if your
system already has the libraries above.

```sh
# Run from a directory outside this repo
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
git checkout ce613c41372b23b1f51333815feb3edd87ef8a8b
./scripts/bootstrap.sh -disableMetrics
export VCPKG_TOOLCHAIN_PATH=$(pwd)/scripts/buildsystems/vcpkg.cmake
```

### 3 — Build

```sh
# Standard release build (recommended for first build)
make release

# Faster: use Ninja + ccache (subsequent builds are much faster)
GEN=ninja make release
```

Artifacts after a successful build:

| Path | Description |
|------|-------------|
| `build/release/duckdb` | DuckDB CLI with the extension **pre-loaded** |
| `build/release/test/unittest` | Test runner with the extension linked in |
| `build/release/extension/quack/quack.duckdb_extension` | Standalone loadable binary |

### Incremental builds

After an initial full build you can rebuild only the extension targets:

```sh
cmake --build build/release \
  --target quack_extension quack_loadable_extension
```

---

## Running the extension

### Option A — Use the bundled DuckDB shell (easiest)

```sh
./build/release/duckdb
```

The extension is pre-loaded; no extra steps are needed.

### Option B — Load into an existing DuckDB installation

```sh
# Start DuckDB in unsigned-extension mode (required for locally built extensions)
duckdb -unsigned
```

```sql
-- Inside the DuckDB shell
LOAD '/path/to/hailing-ducks/build/release/extension/quack/quack.duckdb_extension';
```

Replace `/path/to/hailing-ducks` with the absolute path to this repository.

> **Why `-unsigned`?**  
> DuckDB verifies a cryptographic signature on every extension it loads.
> Locally built extensions are not signed, so the `-unsigned` flag (or the
> `SET allow_unsigned_extensions = true` setting) is required.

### Option C — Load in a client application (Python / R / JDBC …)

```python
import duckdb

con = duckdb.connect(config={"allow_unsigned_extensions": True})
con.execute(
    "LOAD '/path/to/hailing-ducks/build/release/extension/quack/quack.duckdb_extension'"
)
```

---

## Cloud storage (S3 / GCS / HTTP)

Because all file I/O uses DuckDB's VFS layer, cloud paths work out of the box once
the `httpfs` extension is loaded.

### Amazon S3

```sql
INSTALL httpfs;
LOAD httpfs;

-- Set credentials (or configure via environment / instance profile)
SET s3_region = 'us-east-1';
SET s3_access_key_id = '...';
SET s3_secret_access_key = '...';

SELECT * FROM hail_scan_blockmatrix('s3://my-bucket/path/to/matrix.bm') LIMIT 10;
```

### Google Cloud Storage

```sql
INSTALL httpfs;
LOAD httpfs;

-- GCS is accessed via the S3-compatible XML API
SET s3_endpoint = 'storage.googleapis.com';
SET s3_access_key_id = '<HMAC key>';
SET s3_secret_access_key = '<HMAC secret>';

SELECT * FROM hail_scan_blockmatrix('s3://my-gcs-bucket/path/to/matrix.bm') LIMIT 10;
```

### HTTP / HTTPS

```sql
INSTALL httpfs;
LOAD httpfs;

SELECT * FROM hail_scan_blockmatrix('https://example.com/path/to/matrix.bm') LIMIT 10;
```

> **Note:** The `httpfs` extension is not bundled with the debug/test build.
> Load it separately in production DuckDB installations.

---

## Usage

### `hail_scan_blockmatrix(path)`

Scans a Hail BlockMatrix directory and returns one row per matrix element.
The `path` argument accepts any URI scheme supported by the loaded DuckDB
filesystem extensions (`/local/path`, `s3://`, `gs://`, `https://`, …).

```sql
-- Read every element
SELECT * FROM hail_scan_blockmatrix('/data/my_matrix.bm') LIMIT 10;
```

**Output schema**

| Column | Type | Description |
|--------|------|-------------|
| `row_idx` | `BIGINT` | 0-based row index |
| `col_idx` | `BIGINT` | 0-based column index |
| `value`   | `DOUBLE` | Matrix element value |

**Example queries**

```sql
-- Total number of elements
SELECT COUNT(*) FROM hail_scan_blockmatrix('test/matrix.bm');

-- Range of indices
SELECT MIN(row_idx), MAX(row_idx),
       MIN(col_idx), MAX(col_idx)
FROM hail_scan_blockmatrix('test/matrix.bm');

-- Diagonal elements
SELECT row_idx, value
FROM hail_scan_blockmatrix('test/matrix.bm')
WHERE row_idx = col_idx
ORDER BY row_idx;

-- Export to Parquet
COPY (
  SELECT * FROM hail_scan_blockmatrix('/data/betas.bm')
) TO '/data/betas.parquet' (FORMAT PARQUET);
```

---

## Running the tests

```sh
make test
```

Individual SQL test files live in `test/sql/`.  Tests are written in the
[DuckDB SQL test format](https://duckdb.org/docs/dev/testing) and can also
be run directly:

```sh
./build/release/test/unittest --test-dir . [sql]
```

### HTTP integration tests

An additional test suite validates reading over HTTP/HTTPS.  It requires:

1. A DuckDB build that includes `httpfs`
2. Python 3 (for the local test server)

```sh
make test_http
```

This target automatically starts a local Python HTTP server (port 18642),
runs `test/sql/hail_blockmatrix_http.test`, then stops the server.
The test is skipped if `httpfs` is not available in the build.

---

## Project structure

```
hailing-ducks/
├── src/
│   ├── include/
│   │   ├── hail_blockmatrix_scanner.hpp   # BlockMatrix function declaration
│   │   └── quack_extension.hpp            # Extension entry point declaration
│   ├── hail_blockmatrix_scanner.cpp       # BlockMatrix scanner implementation (VFS I/O)
│   └── quack_extension.cpp                # DuckDB extension registration
├── test/
│   ├── sql/
│   │   ├── hail_blockmatrix.test          # BlockMatrix SQL tests (local + error paths)
│   │   ├── hail_blockmatrix_http.test     # HTTP integration test (requires httpfs)
│   │   └── quack.test                     # Baseline extension tests
│   └── matrix.bm/                         # 1000×1000 test fixture (blockSize=4096)
├── scripts/
│   ├── start_test_http_server.sh          # Starts Python HTTP server for integration tests
│   └── stop_test_http_server.sh           # Stops the test HTTP server
├── CMakeLists.txt                          # Build configuration
├── vcpkg.json                              # vcpkg dependency manifest
└── extension_config.cmake                  # DuckDB extension config hook
```

### BlockMatrix on-disk format (brief)

A `.bm` directory contains:

- **`metadata.json`** — `blockSize`, `nRows`, `nCols`, `partFiles`, `maybeFiltered`
- **`parts/<file>`** — LZ4-framed binary blocks  
  Frame layout: `[int32 outer_size][int32 decompressed_size][LZ4 data]`  
  Block payload: `[int32 nRows][int32 nCols][uint8 isTranspose][float64... data]`  
  Data order: column-major when `isTranspose=0`, row-major when `isTranspose=1`

---

## Roadmap

- [x] Phase 1 — BlockMatrix scanner (`hail_scan_blockmatrix`)
- [x] Cloud storage support — S3, GCS, HTTP via DuckDB VFS / `httpfs`
- [ ] Phase 2 — HailTable scanner (`hail_scan_table`)
  - Dynamic schema from `metadata.json.gz`
  - BufferSpec-aware decompression (Zstd, LZ4HC, uncompressed)
  - LEB128 / zigzag integer decoding
  - EStruct missingness bitmask handling

---

## License

[MIT](LICENSE)
