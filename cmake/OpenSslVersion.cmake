function(mcp_validate_openssl_version version)
    if(NOT version MATCHES "^([0-9]+)\\.")
        message(FATAL_ERROR
            "[mcp] Cannot parse OpenSSL version '${version}'. Expected a numeric major version.")
    endif()
    set(_major "${CMAKE_MATCH_1}")
    if(_major LESS 3 OR _major GREATER 4)
        message(FATAL_ERROR
            "[mcp] Unsupported OpenSSL ${version}. Supported major versions are 3 and 4. "
            "Install OpenSSL 3.x or 4.x, or configure with -DMCP_WITH_OPENSSL=OFF.")
    endif()
endfunction()

function(mcp_assert_openssl_major version expected)
    if(expected STREQUAL "")
        return()
    endif()
    if(NOT version MATCHES "^([0-9]+)\\.")
        message(FATAL_ERROR "[mcp] Cannot parse OpenSSL version '${version}'.")
    endif()
    if(NOT "${CMAKE_MATCH_1}" STREQUAL "${expected}")
        message(FATAL_ERROR
            "[mcp] Expected OpenSSL major ${expected}, found ${version}. "
            "Check the OpenSSL installation selected by OPENSSL_ROOT_DIR.")
    endif()
endfunction()
