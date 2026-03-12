cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME quack)

# Add Homebrew prefix so find_package can locate locally installed libs
list(APPEND CMAKE_PREFIX_PATH /opt/homebrew)

# DuckDB's extension distribution supports vcpkg. As such, dependencies can be added in ./vcpkg.json and then
# used in cmake with find_package. Feel free to remove or replace with other dependencies.
# Note that it should also be removed from vcpkg.json to prevent needlessly installing it..
find_package(OpenSSL REQUIRED)

# lz4: try cmake config first (vcpkg), fall back to plain find_path/find_library (Homebrew)
find_package(lz4 CONFIG QUIET)
if(NOT lz4_FOUND)
    find_path(LZ4_INCLUDE_DIR lz4.h REQUIRED)
    find_library(LZ4_LIBRARY NAMES lz4 liblz4 REQUIRED)
    add_library(lz4::lz4 UNKNOWN IMPORTED)
    set_target_properties(lz4::lz4 PROPERTIES
        IMPORTED_LOCATION "${LZ4_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LZ4_INCLUDE_DIR}"
    )
endif()

# nlohmann-json: header-only, cmake config available via Homebrew/vcpkg
find_package(nlohmann_json CONFIG REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(src/include)

set(EXTENSION_SOURCES
    src/quack_extension.cpp
    src/hail_blockmatrix_scanner.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Link dependencies in both the static library and the loadable extension
target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto lz4::lz4 nlohmann_json::nlohmann_json)
target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto lz4::lz4 nlohmann_json::nlohmann_json)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
