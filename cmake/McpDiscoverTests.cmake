include(GoogleTest)

function(mcp_discover_tests target)
    gtest_discover_tests(${target}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        PROPERTIES TIMEOUT 120
        DISCOVERY_TIMEOUT 60)
endfunction()
