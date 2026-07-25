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
    find_path(LZ4_INCLUDE_DIR lz4.h QUIET)
    find_library(LZ4_LIBRARY NAMES lz4 liblz4 QUIET)
    if(LZ4_INCLUDE_DIR AND LZ4_LIBRARY)
        add_library(lz4::lz4 UNKNOWN IMPORTED)
        set_target_properties(lz4::lz4 PROPERTIES
            IMPORTED_LOCATION "${LZ4_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${LZ4_INCLUDE_DIR}"
        )
    else()
        message(WARNING "LZ4 not found; building without LZ4 support (hail_blockmatrix_scanner will be limited)")
        add_library(lz4::lz4 INTERFACE IMPORTED)
    endif()
endif()

# nlohmann-json: header-only, try cmake config (vcpkg/Homebrew) then fall back to downloading the single header
find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    set(NLOHMANN_JSON_INCLUDE_FALLBACK "${CMAKE_CURRENT_BINARY_DIR}/include")
    set(NLOHMANN_JSON_HEADER "${NLOHMANN_JSON_INCLUDE_FALLBACK}/nlohmann/json.hpp")
    if(NOT EXISTS "${NLOHMANN_JSON_HEADER}")
        message(STATUS "nlohmann_json not found via config; downloading single-header fallback")
        file(MAKE_DIRECTORY "${NLOHMANN_JSON_INCLUDE_FALLBACK}/nlohmann")
        file(DOWNLOAD
            "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp"
            "${NLOHMANN_JSON_HEADER}"
            STATUS NLOHMANN_DOWNLOAD_STATUS
        )
        list(GET NLOHMANN_DOWNLOAD_STATUS 0 NLOHMANN_STATUS_CODE)
        if(NOT NLOHMANN_STATUS_CODE EQUAL 0)
            message(FATAL_ERROR "Failed to download nlohmann/json single header")
        endif()
    endif()
    add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED)
    set_target_properties(nlohmann_json::nlohmann_json PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${NLOHMANN_JSON_INCLUDE_FALLBACK}"
    )
endif()

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(src/include)

set(EXTENSION_SOURCES
    src/quack_extension.cpp
    src/hail_codec.cpp
    src/hail_type_parser.cpp
)

# Only include hail_blockmatrix_scanner if LZ4 is available
if(lz4_FOUND OR (LZ4_INCLUDE_DIR AND LZ4_LIBRARY))
    list(APPEND EXTENSION_SOURCES src/hail_blockmatrix_scanner.cpp)
endif()

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Link dependencies in both the static library and the loadable extension
target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto nlohmann_json::nlohmann_json)
target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto nlohmann_json::nlohmann_json)

# Conditionally link lz4 if available
if(lz4_FOUND OR (LZ4_INCLUDE_DIR AND LZ4_LIBRARY))
    target_link_libraries(${EXTENSION_NAME} lz4::lz4)
    target_link_libraries(${LOADABLE_EXTENSION_NAME} lz4::lz4)
    target_compile_definitions(${EXTENSION_NAME} PRIVATE HAVE_LZ4)
    target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE HAVE_LZ4)
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
