# Repository Guidelines

## Project Structure & Module Organization

This repository builds the `quack` DuckDB extension for reading Hail native files without JVM dependencies. Core C++ implementation files live in `src/`, with public/internal declarations in `src/include/`. Extension registration starts in `src/quack_extension.cpp`; Hail BlockMatrix scanning is in `src/hail_blockmatrix_scanner.cpp`; HailTable scanning is in `src/hail_table_scanner.cpp`; codec helpers are in `src/hail_codec.cpp`. `src/hail_ld_query.cpp` implements a separate, purpose-built batch-optimized LD query pipeline (resolving a fine-mapping locus's requested variants against a real HailTable and extracting exactly the LD pairs among them from a real BlockMatrix) — see `docs/LD-QUERY.md` for the full reference.

Tests and fixtures live under `test/`. SQLLogicTest files are in `test/sql/`, binary codec fixtures are in `test/codec/`, and sample Hail datasets are stored as fixture directories such as `test/matrix.bm/`, `test/variant_indices.ht/`, and the LD-query-specific `test/matrix_ld.bm/`/`test/hailtable_fixture_ld.ht/`. Fixtures are committed binaries generated offline by the scripts in `scripts/` (no Hail dependency needed to regenerate them). Build metadata is in `CMakeLists.txt`, `extension_config.cmake`, `vcpkg.json`, and the root `Makefile`.

## Build, Test, and Development Commands

- `git submodule update --init --recursive`: populate DuckDB extension tooling before the first build.
- `make release`: build the release DuckDB CLI, unit test runner, and loadable extension.
- `GEN=ninja make release`: build with Ninja when available for faster incremental builds.
- `cmake --build build/release --target quack_extension quack_loadable_extension`: rebuild only extension targets after a full configure.
- `make test`: run the SQLLogicTest suite.
- `make test_debug`: run tests against a debug build.
- `make test_http`: start the local HTTP fixture server and run HTTP-backed BlockMatrix tests.
- `make test_s3_smoke`: opt-in real-S3 LD query smoke test against PanUKBB-style HT/BM paths; skips cleanly unless `HAILING_DUCKS_S3_HT_PATH`/`HAILING_DUCKS_S3_BM_PATH` are set (no AWS credentials needed for the public PanUKBB bucket).

## Coding Style & Naming Conventions

Use C++17 and follow DuckDB extension conventions. Keep declarations in `src/include/*.hpp` and implementation in matching `src/*.cpp` files. Use `snake_case` for functions and local variables, `PascalCase` for classes and structs, and clear names for table functions such as `hail_scan_blockmatrix`. Format C++ changes with the repository clang-format configuration when the DuckDB submodule is present.

## Testing Guidelines

Prefer SQLLogicTests for extension behavior; add new `.test` files under `test/sql/` or extend focused existing files. Keep fixtures small, deterministic, and checked in under `test/`. For codec edge cases, add binary fixtures under `test/codec/` with SQL coverage that proves decoding, boundary, and error behavior.

## Commit & Pull Request Guidelines

Recent history uses short imperative subjects, sometimes with conventional prefixes such as `style:` or `feat:`. Write subjects that explain the intent, for example `Fix LEB128 overflow protection`. PRs should include a concise description, test evidence such as `make test`, linked issues when applicable, and notes for fixture, dependency, or build-system changes.
