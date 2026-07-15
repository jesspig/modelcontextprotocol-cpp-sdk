include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ====================================================================
# Target installation
# ====================================================================
install(TARGETS
    mcp-protocol
    mcp-server
    mcp-client
    mcp-http
    EXPORT mcp-cpp-sdk-targets
    RUNTIME COMPONENT Runtime
    LIBRARY COMPONENT Runtime
    ARCHIVE COMPONENT Runtime
    PUBLIC_HEADER COMPONENT Development)

# Header-only / interface libraries with external dependencies
install(TARGETS
    mcp-core
    mcp-transport
    EXPORT mcp-cpp-sdk-targets)

# External dependencies needed by the export set
install(TARGETS nlohmann_json asio
    EXPORT mcp-cpp-sdk-targets)

# ====================================================================
# Public headers
# ====================================================================
install(DIRECTORY include/mcp/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mcp
    COMPONENT Development)

# ====================================================================
# CMake package config
# ====================================================================
set(MCP_CONFIG_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/mcp-cpp-sdk")

install(EXPORT mcp-cpp-sdk-targets
    FILE mcp-cpp-sdk-targets.cmake
    NAMESPACE mcp::
    DESTINATION ${MCP_CONFIG_INSTALL_DIR}
    COMPONENT Development)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/mcp-cpp-sdk-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mcp-cpp-sdk-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/mcp-cpp-sdk-config.cmake"
    INSTALL_DESTINATION ${MCP_CONFIG_INSTALL_DIR})

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/mcp-cpp-sdk-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/mcp-cpp-sdk-config-version.cmake"
    DESTINATION ${MCP_CONFIG_INSTALL_DIR}
    COMPONENT Development)
