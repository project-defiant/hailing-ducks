# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

`hailing-ducks` (extension name: `quack`) is a DuckDB extension that reads [Hail](https://hail.is/)
native binary file formats directly — no JVM, Spark, or Python Hail package required. It registers
DuckDB table functions so Hail data can be queried in SQL, joined with other tables, and exported to
any format DuckDB supports. Built on the standard [duckdb/extension-template](https://github.com/duckdb/extension-template).

Supported formats:
- **BlockMatrix** (`<name>.bm/`) — `hail_scan_blockmatrix(path)`, implemented (Phase 1).
- **HailTable** (`<name>.ht/`) — `hail_scan_table(path)`, implemented (Phase 2) with metadata-driven
  schemas, nested `LIST`/`STRUCT` output, and Zstd/LZ4HC/LZ4Fast row decoding.

## Build commands

This repo uses git submodules for `duckdb` and `extension-ci-tools` — run
`git submodule update --init --recursive` before building if they're not populated.

```sh
make release              # standard release build (recommended first build)
GEN=ninja make release    # much faster incremental rebuilds via Ninja + ccache
make debug                # debug build

# After an initial full build, rebuild only the extension's own targets:
cmake --build build/release --target quack_extension quack_loadable_extension
```

Build artifacts:
| Path | Description |
|------|-------------|
| `build/release/duckdb` | DuckDB CLI with the extension pre-loaded |
| `build/release/test/unittest` | Test runner with the extension linked in |
| `build/release/extension/quack/quack.duckdb_extension` | Standalone loadable binary |

Non-vcpkg dependencies (nlohmann-json, OpenSSL) are resolved via `find_package`/Homebrew paths in
`CMakeLists.txt`, falling back to vcpkg or a downloaded single-header for `nlohmann/json` if not
found locally. LZ4 is built from DuckDB's bundled `duckdb/third_party/lz4` source, not from vcpkg or
a system `find_package(lz4)`.

## Test commands

```sh
make test                                              # run the full SQLLogicTest suite
./build/release/test/unittest --test-dir . [sql]       # run tests directly
./build/release/test/unittest --test-dir . "test/sql/hail_codec.test"   # run a single test file
make test_http                                         # HTTP/VFS integration test (starts/stops a
                                                        # local Python HTTP server around the run)
```

Tests are [DuckDB SQLLogicTests](https://duckdb.org/docs/dev/testing) living in `test/sql/*.test`.
Each starts with `require quack`. Notes on the existing suite:
- `test/sql/hail_blockmatrix.test` also exercises the absolute-path VFS case via
  `${HAILING_DUCKS_ROOT}` (exported by the root `Makefile`, no trailing slash) and
  `require-env HAILING_DUCKS_ROOT`.
- `test/sql/hail_blockmatrix_http.test` requires `httpfs` and `HTTP_TEST_PORT`; only run through
  `make test_http`, not plain `make test`.
- `test/sql/hail_codec.test` exercises `ZstdBlockDecoder` against fixed binary fixtures in
  `test/codec/*.bin` and `test/variant_indices.ht/…`, asserting exact byte-level round trips (e.g.
  known LEB128 values including `UINT32_MAX`/`UINT64_MAX` boundary cases).
- Test fixtures under `test/` (e.g. `test/matrix.bm/`, a 1000×1000 matrix with `blockSize=4096`) are
  committed binary data, not generated at test time — new format coverage needs a matching fixture.

CI (`.github/workflows/MainDistributionPipeline.yml`) also runs a code-quality job against
`clang-format`/`clang-tidy` (`format_checks: 'format;tidy'`), using the repo's `.clang-format` /
`.clang-tidy` (symlinked from the `duckdb` submodule).

## Architecture

### Extension entry point

`src/quack_extension.cpp` is the sole registration point (`QuackExtension::Load` /
`DUCKDB_CPP_EXTENSION_ENTRY`). Every new table/scalar function must be registered here by calling
that function's `::Register(loader)`, mirroring `HailBlockMatrixScanFunction::Register` and
`HailCodecScanFunction::Register`.

### Table function pattern

Both Hail scanners follow DuckDB's standard table-function shape — bind → init_global (parallelism)
→ init_local (per-thread state) → scan — with types split as:
- `*BindData : TableFunctionData` — parsed, immutable per-query config (paths, schema, metadata).
- `*GlobalState : GlobalTableFunctionState` — cross-thread coordination, e.g.
  `HailBlockMatrixGlobalState` hands out partition indices via `std::atomic<idx_t> next_part`, and
  `MaxThreads()` is capped at the partition count.
- `*LocalState : LocalTableFunctionState` — per-thread scan cursor/decoder instance.

All file I/O goes through DuckDB's `FileSystem`/`FileHandle` VFS abstraction (never raw `fopen`/`ifstream`),
so local, S3, GCS, and HTTP paths work transparently. Reads loop over partial reads (`read_exact`-style
helpers) since this is expected behavior for HTTP/cloud backends, not just a defensive check.

### `hail_blockmatrix_scanner.cpp` — BlockMatrix scanner

Reads a `.bm` directory: `metadata.json` (`blockSize`, `nRows`, `nCols`, `partFiles`, optional
`maybeFiltered` for filtered/reordered partitions) drives a `GridPartitioner`
(`ComputeBlockInfo`) that maps a flat partition/block index to global row/col offsets. Each
`parts/<file>` is a repeating LZ4-framed stream (`DecompressHailLz4Stream`):
`[int32 outer_frame_size][int32 decompressed_size][LZ4 data]`, decompressing to a block payload of
`[int32 nRows][int32 nCols][uint8 isTranspose][float64 data…]` — column-major when
`isTranspose=0`, row-major when `isTranspose=1`. Output schema: `(row_idx BIGINT, col_idx BIGINT,
value DOUBLE)`, one row per matrix element. Partition filenames from `metadata.json` are validated
against path traversal (`..`, `/`, `\`) before being joined onto the base path.

### `hail_codec.cpp` / `hail_codec.hpp` — Hail codec stack

`BlockDecoder` is the shared scanner-facing interface for HailTable part files. The factory walks
the metadata `BufferSpec` chain and dispatches to Zstd or LZ4; both `LZ4HCBlockBufferSpec` and
`LZ4FastBlockBufferSpec` use DuckDB's bundled LZ4 implementation. The decoded stream exposes
`read_byte`, `read_leb128_u32`/`read_leb128_u64`
(overflow-guarded at 5/10 bytes), `read_float`/`read_double`, and `read_bytes`/`skip_bytes` (bulk,
cross-block-boundary aware). `eof()` is maintained eagerly — the next frame header is probed as soon
as the current block is exhausted, so it's accurate without an extra read call.

The three registered functions here (`hail_zstd_info`, `hail_leb128_u32`, `hail_leb128_u64`) exist
purely as **testing helpers** for `ZstdBlockDecoder` itself — they are not part of the public HailTable
scanning API. `hail_scan_table` consumes the shared decoder abstraction directly instead of
re-deriving frame, LEB128, or LZ4/Zstd logic.

## Updating the DuckDB target version

Bumping the DuckDB version touches multiple places at once — see `docs/UPDATING.md` for the full
procedure: pin `duckdb`/`extension-ci-tools` submodules to matching tags, and update the
`duckdb_version`/`ci_tools_version` inputs in `.github/workflows/MainDistributionPipeline.yml`.
