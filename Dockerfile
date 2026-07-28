# Builds a DuckDB CLI image with the `quack` (Hail reader) and `httpfs` extensions already loaded.
#
# Build context must be a fully checked-out working tree (git submodules populated -- `duckdb/` and
# `extension-ci-tools/` need real file content, not just submodule pointers), since this build does
# not run `git submodule update` itself. Locally:
#   git submodule update --init --recursive
#   docker build -t hailing-ducks .
#   docker run --rm -it hailing-ducks
#
# `httpfs` is dynamically linked against libcurl/libssl at build time (see extension_config.cmake),
# so the runtime stage installs the matching shared libraries even though both extensions are
# otherwise statically linked into the `duckdb` binary itself -- no INSTALL/LOAD needed at runtime.

FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git python3 \
        liblz4-dev nlohmann-json3-dev libssl-dev libcurl4-openssl-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN GEN=ninja make release

FROM ubuntu:24.04 AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
        libcurl4 libssl3 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/release/duckdb /usr/local/bin/duckdb
COPY --from=builder /src/build/release/extension/quack/quack.duckdb_extension /opt/duckdb_extensions/quack.duckdb_extension
COPY --from=builder /src/build/release/extension/httpfs/httpfs.duckdb_extension /opt/duckdb_extensions/httpfs.duckdb_extension

WORKDIR /data
ENTRYPOINT ["duckdb"]
