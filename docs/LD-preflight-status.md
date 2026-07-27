LD preflight / hail_ld work — current status

Summary
- Implemented hail_ld_preflight table-function and helpers under TDD.
- Added hail_ld_parse_token scalar and hail_ld_preflight_debug table for parsing inspection.
- Implemented parsing, normalization, dedupe, grouping, and classification rules (codes 0..6).

What works
- Scalar helper adjusted and verified (test: hail_ld_parse_token.test).
- Many parsing & chunking behaviors implemented; several SQLLogicTests pass (partial suite).

Remaining blockers
1) hail_ld_preflight_debug crashes under SQLLogicTest with an internal DuckDB error: "Attempting to dereference an optional pointer that is not set". Root cause: table-function bind/init path differences and missing bind_data in some planner paths.
2) Debugging on this machine hit an AArch64 linker CALL26 relocation overflow when building a Debug unittest with sanitizers — prevents producing a symbolized backtrace in the usual way.
3) Multiple hail_ld_*.test cases still failing; some failures look tied to trimming/contig parsing and outside-locus classification.

Work done so far (commits)
- Fixed scalar output format and updated its test.
- Hardened contig-join loops and debug emission to avoid vector deref assertions.
- Added logging to trace bind/init behavior for the debug table.

Next recommended steps
- Rebuild a smaller debug test binary (no sanitizers) to obtain a symbolized stack trace and locate the optional-pointer deref site. Command suggestion included below.
- Once failing site is found, fix HailLDPreflightInitLocalDebug to use the correct TableFunctionInitInput APIs and ensure bind_data is always available, or make init code robust to missing bind_data.
- Iterate failing tests one-by-one: fix trimming/contig parsing edge cases, run tests, and remove test-only debug artifacts before finalizing.

Commands to run locally (recommended):
- Quick run (no debug symbols): ./build/release/test/unittest --test-dir . "test/sql/hail_ld_preflight_debug.test"
- Build smaller debug runner (no sanitizers):
  mkdir -p build/debug-small
  cmake -G Ninja -S duckdb -B build/debug-small -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=OFF -DCMAKE_C_FLAGS="-g -O0" -DCMAKE_CXX_FLAGS="-g -O0" -DDUCKDB_EXTENSION_CONFIGS="${PWD}/extension_config.cmake"
  cmake --build build/debug-small --target unittest -j$(nproc)
  gdb --args build/debug-small/unittest --test-dir . "test/sql/hail_ld_preflight_debug.test"

If you want, I can also open issues for the specific failing test files and triage them individually.