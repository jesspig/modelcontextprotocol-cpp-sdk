# McpDiscoverTests.cmake — 逐用例 CTest 注册（自研测试框架）
function(mcp_discover_tests target)
    set(tests_file "${CMAKE_CURRENT_BINARY_DIR}/${target}[1]_tests.cmake")
    set(include_file "${CMAKE_CURRENT_BINARY_DIR}/${target}[1]_include.cmake")
    file(WRITE "${include_file}"
        "if(EXISTS \"${tests_file}\")\n  include(\"${tests_file}\")\nelse()\n  add_test(${target}_NOT_BUILT ${target}_NOT_BUILT)\nendif()\n")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -D TARGET_EXE=$<TARGET_FILE:${target}>
            -D TESTS_FILE=${tests_file}
            -P ${CMAKE_SOURCE_DIR}/cmake/McpDiscoverTests.cmake.script
        VERBATIM)
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${include_file}")
endfunction()
