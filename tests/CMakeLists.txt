include_directories(${CMAKE_SOURCE_DIR}/src)

add_library(mcp-test-support INTERFACE)
target_include_directories(mcp-test-support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/support/include)

add_subdirectory(unit)

add_subdirectory(bench)

add_subdirectory(integration)
add_subdirectory(conformance)
add_subdirectory(compat)
