# ====================================================================
# mcp-cpp-sdk 安装规则
# ====================================================================
include(GNUInstallDirs)

install(TARGETS
    mcp-core
    mcp-transport
    mcp-protocol
    mcp-server
    mcp-client
    mcp-http
    EXPORT mcp-cpp-sdk-targets
    RUNTIME COMPONENT Runtime
    LIBRARY COMPONENT Runtime
    ARCHIVE COMPONENT Runtime
    PUBLIC_HEADER COMPONENT Development)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT Development)

install(EXPORT mcp-cpp-sdk-targets
    FILE mcp-cpp-sdk-config.cmake
    NAMESPACE mcp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mcp-cpp-sdk
    COMPONENT Development)
