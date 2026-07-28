# hailing-ducks

A DuckDB extension that reads [Hail](https://hail.is/) native binary file formats
**without a JVM** — no Spark, no Python Hail package required.

The extension registers DuckDB table functions so you can query Hail data directly
in SQL, join it with other tables, and export it to any format DuckDB supports.

## Supported formats

| Hail type | File pattern | DuckDB function |
|-----------|--------------|-----------------|
| BlockMatrix | `<name>.bm/` | `hail_scan_blockmatrix(path)` |
| HailTable | `<name>.ht/` | `hail_scan_table(path)` |

The two functions above are raw, full sequential scanners — they read every row/element and are
the right tool for inspection, conversion, or joining Hail data with other tables in SQL.

For resolving a fine-mapping locus's requested variants against a real HailTable and extracting
exactly the LD pairs among them from a real BlockMatrix — without scanning either file in full —
see **[Batch-optimized LD query](#batch-optimized-ld-query)** below.

Runnable examples for both live in **[examples/](examples/)**.

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

## Usage

### `hail_scan_blockmatrix(path)`

Scans a Hail BlockMatrix directory and returns one row per matrix element.

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

### `hail_scan_table(path)`

Scans a Hail `.ht` (HailTable) directory and returns one row per table row,
with the DuckDB schema derived from the table's Hail type metadata.

```sql
-- Read every row
SELECT * FROM hail_scan_table('/data/my_table.ht') LIMIT 10;
```

Nested Hail `Array` and `Struct` fields become DuckDB `LIST` and `STRUCT`
columns. `Locus(GENOME)` decodes to `STRUCT(contig VARCHAR, position INTEGER)`.
The scanner does not perform liftover; loci are returned exactly as encoded in
the source table's reference genome.

**Example queries**

```sql
-- Row count
SELECT COUNT(*) FROM hail_scan_table('test/hailtable_fixture.ht');

-- Nested struct field access
SELECT locus.contig, locus.position
FROM hail_scan_table('test/hailtable_fixture.ht');

-- List field
SELECT alleles FROM hail_scan_table('test/hailtable_fixture.ht');
```

---

## Batch-optimized LD query

A separate, purpose-built pipeline (distinct from the raw scanners above) resolves a
fine-mapping locus's requested variants against a real HailTable using locus-pruned
direct/flipped allele matching, then extracts exactly the LD pairs among them from a real
BlockMatrix using a shared, capacity-bounded block cache — all without scanning either file in
full and without Hail, Spark, or a JVM.

**Full reference:** pipeline diagram, function signatures, request/output schemas, the complete
status-code table, and policies (no normalization, biallelic-only, no diagonal pairs) live in
**[docs/LD-QUERY.md](docs/LD-QUERY.md)**.

### Quick example (using the committed test fixtures)

```sql
-- 1. Build a request file: one row per locus, with the variant IDs you want LD for.
COPY (
  SELECT * FROM (VALUES
    ('locus1', 'chr1:100-500', ['chr1_200_A_G', 'chr1_300_A_G'])
  ) AS t(locus_id, locus, variant_ids)
) TO 'requests.parquet' (FORMAT PARQUET);

-- 2. Resolve requested variants against the HailTable (direct + flipped allele matching).
SELECT * FROM hail_ld_resolve_ht('test/hailtable_fixture_ld.ht', 'requests.parquet');

-- 3. One-shot materialize: writes ld_pairs.parquet (successful pairs only) and
--    variant_resolution_status.parquet (one row per unique requested variant, incl. failures).
SELECT * FROM hail_ld_materialize(
  'test/hailtable_fixture_ld.ht', 'test/matrix_ld.bm', 'requests.parquet',
  'ld_pairs.parquet', 'variant_resolution_status.parquet'
);
```

`hail_ld_materialize` streams both outputs directly to Parquet (via a `duckdb::Appender`-backed
staging table), so peak memory stays bounded regardless of how many loci or LD pairs a single
combined request contains — verified against a real 38.5M-pair, 24-locus combined PanUKBB request.

### Real S3 data

```sh
HAILING_DUCKS_S3_HT_PATH=s3://pan-ukb-us-east-1/ld_release/UKBB.EUR.ldadj.variant.b38.ht \
HAILING_DUCKS_S3_BM_PATH=s3://pan-ukb-us-east-1/ld_release/UKBB.EUR.ldadj.bm \
make test_s3_smoke
```

Needs no AWS credentials (the PanUKBB bucket is public/unsigned); skips cleanly if the env vars
aren't set. See [docs/LD-QUERY.md](docs/LD-QUERY.md#running-the-s3-smoke-test) for details.

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

Two additional opt-in targets exercise `httpfs`-backed remote reads and are not part of plain
`make test` (no network access required for the default suite):

```sh
make test_http       # local HTTP server + BlockMatrix-over-HTTP integration test
make test_s3_smoke   # opt-in real-S3 LD query smoke test (see above)
```

---

## Project structure

```
hailing-ducks/
├── src/
│   ├── include/                            # Public/internal declarations (.hpp per .cpp below)
│   ├── quack_extension.cpp                # Extension entry point / function registration
│   ├── hail_blockmatrix_scanner.cpp       # BlockMatrix scanner (hail_scan_blockmatrix)
│   ├── hail_table_scanner.cpp             # HailTable scanner (hail_scan_table)
│   ├── hail_type_parser.cpp               # Hail VType/EType parser
│   ├── hail_codec.cpp                     # Shared Zstd/LZ4 block-decoder stack
│   └── hail_ld_query.cpp                  # Batch-optimized LD query pipeline (see above)
├── test/
│   ├── sql/                                # SQLLogicTest files (one concern per file)
│   ├── hailtable_fixture.ht/               # Synthetic HailTable fixture
│   ├── hailtable_fixture_ld.ht/            # HailTable fixture for the LD query pipeline
│   ├── matrix.bm/                          # 1000×1000 BlockMatrix fixture (blockSize=4096)
│   └── matrix_ld.bm/                       # BlockMatrix fixture for the LD query pipeline
├── scripts/                                 # Offline fixture generators (no Hail dependency)
├── examples/                                 # Runnable .sql walkthroughs (see examples/README.md)
├── docs/
│   ├── LD-QUERY.md                        # Full LD query pipeline reference
│   └── UPDATING.md                        # Procedure for bumping the DuckDB target version
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
- [x] Phase 2 — HailTable scanner (`hail_scan_table`)
  - [x] Dynamic schema from `metadata.json.gz`
  - [x] BufferSpec-aware decompression (Zstd, LZ4HC, LZ4Fast)
  - [x] Unsigned LEB128 integer decoding
  - [x] Nested `LIST`/`STRUCT` output with missingness bitmask handling
- [x] Phase 3 — Batch-optimized LD query pipeline (see
  [above](#batch-optimized-ld-query), full detail in [docs/LD-QUERY.md](docs/LD-QUERY.md))
  - [x] Request preflight, status codes, event schema
  - [x] Locus-pruned HailTable resolver (direct + flipped allele matching)
  - [x] BlockMatrix pair extraction with a shared, capacity-bounded block cache
  - [x] Multi-locus batching and query-local block cache reuse
  - [x] One-pass materializer (`hail_ld_materialize`), memory-bounded regardless of request size
  - [x] Real-S3 smoke testing against PanUKBB HT/BM data

Known gaps (raw scanners only — the LD query pipeline has its own, narrower scope documented in
[docs/LD-QUERY.md](docs/LD-QUERY.md#policies)): uncompressed HailTable codecs, globals, and generic
key-index lookups are not implemented for `hail_scan_table`, which targets full sequential row scans.

---

## License

[MIT](LICENSE)
