# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(quack
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# Any extra extensions that should be built
# e.g.: duckdb_extension_load(json)

# httpfs: needed by test/sql/hail_blockmatrix_http.test (make test_http) and
# test/sql/hail_ld_s3_smoke.test (make test_s3_smoke), both of which `require httpfs`. Pinned to the
# same GIT_TAG this vendored duckdb submodule itself uses (duckdb/.github/config/extensions/httpfs.cmake).
# Deliberately no LOAD_TESTS here -- that would pull httpfs's own bundled SQLLogicTest suite into this
# project's test surface (extra require-json/require-tpch skips this project doesn't need).
#
# Skipped entirely for clang-tidy runs (CLANG_TIDY is the cache variable
# extension-ci-tools' `make tidy-check` sets): tidy-check only lints this repo's own quack sources,
# never touches httpfs, and the CI runner it uses has neither libcurl nor libssl dev headers
# installed, which httpfs's own CMakeLists.txt unconditionally requires to even configure.
if(NOT CLANG_TIDY)
    duckdb_extension_load(httpfs
        GIT_URL https://github.com/duckdb/duckdb-httpfs
        GIT_TAG 827222fb45a043a7a852d1f7aae46901492a3cda
    )
endif()