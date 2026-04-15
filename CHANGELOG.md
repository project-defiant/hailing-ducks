# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Added

- **`ZstdBlockDecoder`** (`src/hail_codec.hpp` / `src/hail_codec.cpp`): streaming
  decoder for the Hail codec stack used by HailTable part files.  Reads
  `StreamBlockBufferSpec` outer frames and `ZstdBlockBufferSpec` inner frames on
  top of DuckDB's VFS `FileHandle`, transparently supporting local, S3, GCS and
  HTTP backends with partial-read retry logic.
  - `read_byte()` — single byte; maintains eager `eof()` state
  - `read_leb128_u32()` / `read_leb128_u64()` — unsigned LEB128 (Hail
    convention, not zigzag); overflow-guarded at 5 / 10 bytes
  - `read_float()` / `read_double()` — raw IEEE 754 via `memcpy`
  - `read_bytes()` / `skip_bytes()` — bulk operations with cross-block boundary
    support
  - Uses `duckdb_zstd::ZSTD_decompress` from DuckDB's bundled zstd — no new
    external dependency
- **SQL test helpers** registered as DuckDB table functions:
  - `hail_zstd_info(path)` → `(frame_idx INTEGER, decomp_size INTEGER)`:
    inspects raw stream frames and returns the decompressed size per frame
  - `hail_leb128_u32(path)` / `hail_leb128_u64(path)`: count-prefixed LEB128
    round-trip readers for unit testing
- **Test data** under `test/`:
  - `test/variant_indices.ht/rows/parts/part-00000-dc70bdc7-fa3a-4c4a-b60f-324b0e34b4d6`:
    synthetic single-frame file with `decomp_size = 30145`
  - `test/codec/leb128_u32.bin` / `test/codec/leb128_u64.bin`: exact-fit
    encoded files for LEB128 round-trip verification (EOF after last value)
- **`test/sql/hail_codec.test`**: SQLLogicTest suite covering frame metadata,
  LEB128 u32/u64 round-trips (including boundary values such as `UINT32_MAX` and
  `UINT64_MAX`), and EOF behaviour
