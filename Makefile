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
	cmd //C "copy /Y build\$(1)\src\*.dll build\$(1)\test\ >NUL 2>NUL & set PATH=%CD%\build\$(1)\src;%PATH%& build\$(1)\test\unittest.exe $(WINDOWS_TEST_PATTERN)"
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
