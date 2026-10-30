include("${CMAKE_CURRENT_LIST_DIR}/../cmake/OpenSslVersion.cmake")

if(NOT DEFINED OPENSSL_VERSION)
    message(FATAL_ERROR "[mcp] OPENSSL_VERSION is required")
endif()

mcp_validate_openssl_version("${OPENSSL_VERSION}")
