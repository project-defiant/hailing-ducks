PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=quack
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

ifneq (,$(filter windows_%,$(DUCKDB_PLATFORM)))
TEST_PATH=test/unittest.exe
WINDOWS_TEST_PATTERN=$(subst ",,$(TESTS_BASE_DIRECTORY))*

define RUN_WINDOWS_UNITTEST
	powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -Command "$$ErrorActionPreference = 'Stop'; Copy-Item -Force 'build/$(1)/src/*.dll' 'build/$(1)/test/' -ErrorAction SilentlyContinue; $$env:PATH = (Resolve-Path 'build/$(1)/src').Path + ';' + $$env:PATH; & 'build/$(1)/test/unittest.exe' '$(WINDOWS_TEST_PATTERN)'; exit $$LASTEXITCODE"
endef

test_release_internal:
	$(call RUN_WINDOWS_UNITTEST,release)
test_debug_internal:
	$(call RUN_WINDOWS_UNITTEST,debug)
test_reldebug_internal:
	$(call RUN_WINDOWS_UNITTEST,reldebug)
endif

# Export the project root so SQLLogicTests can substitute ${HAILING_DUCKS_ROOT} in path expressions
export HAILING_DUCKS_ROOT := $(CURDIR)

# Run HTTP integration tests — starts a local Python HTTP server, requires httpfs to be available.
# Usage: make test_http
.PHONY: test_http
test_http: release
	scripts/start_test_http_server.sh
	sleep 1
	HTTP_TEST_PORT=18642 ./build/release/$(TEST_PATH) "test/sql/hail_blockmatrix_http.test" || \
	  { scripts/stop_test_http_server.sh; exit 1; }
	scripts/stop_test_http_server.sh

# Run the opt-in S3 smoke test against a real, remote PanUKBB-style HailTable + BlockMatrix (issue
# #22). Requires httpfs and network access; skips cleanly (not a failure) if the path env vars below
# aren't set, so plain `make test` never depends on network access. No AWS credentials are needed for
# the public PanUKBB bucket.
# Usage:
#   HAILING_DUCKS_S3_HT_PATH=s3://pan-ukb-us-east-1/ld_release/UKBB.EUR.ldadj.variant.b38.ht \
#   HAILING_DUCKS_S3_BM_PATH=s3://pan-ukb-us-east-1/ld_release/UKBB.EUR.ldadj.bm \
#   make test_s3_smoke
.PHONY: test_s3_smoke
test_s3_smoke: release
	./build/release/$(TEST_PATH) --test-dir . "test/sql/hail_ld_s3_smoke.test"
