PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=quack
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

ifneq (,$(filter windows_%,$(DUCKDB_PLATFORM)))
	TEST_PATH=test/unittest.exe
	export PATH := ./build/release/src:./build/debug/src:./build/reldebug/src:$(PATH)
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
