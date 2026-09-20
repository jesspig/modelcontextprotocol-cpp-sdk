cmake_minimum_required(VERSION 3.28...4.2)

if(NOT DEFINED CACHE{CMAKE_CXX_COMPILER})
    if(WIN32)
        find_program(MCP_CLANG_CL NAMES clang-cl
            PATHS "C:/Program Files/LLVM/bin"
                  "C:/Program Files (x86)/LLVM/bin")
        if(MCP_CLANG_CL)
            set(CMAKE_C_COMPILER "${MCP_CLANG_CL}")
            set(CMAKE_CXX_COMPILER "${MCP_CLANG_CL}")
            set(CMAKE_CXX_FLAGS "--target=x86_64-pc-windows-msvc" CACHE STRING "")
            message(STATUS "[mcp] Auto-detected: clang-cl (${MCP_CLANG_CL})")
        else()
            message(STATUS "[mcp] No explicit compiler set, using system default")
        endif()
    else()
        find_program(MCP_CLANGXX NAMES clang++-19 clang++-18 clang++-17 clang++-16 clang++)
        if(MCP_CLANGXX)
            set(CMAKE_CXX_COMPILER "${MCP_CLANGXX}")
            get_filename_component(_cxx_name "${MCP_CLANGXX}" NAME)
            if(_cxx_name MATCHES "^clang\\+\\+-(.+)$")
                set(_ver "${CMAKE_MATCH_1}")
                find_program(MCP_CLANG NAMES "clang-${_ver}" "clang")
            else()
                find_program(MCP_CLANG NAMES "clang")
            endif()
            if(MCP_CLANG)
                set(CMAKE_C_COMPILER "${MCP_CLANG}")
                message(STATUS "[mcp] Auto-detected: clang++ / clang (${MCP_CLANGXX}, ${MCP_CLANG})")
            else()
                message(STATUS "[mcp] Auto-detected: clang++ (${MCP_CLANGXX}), C compiler using system default")
            endif()
        else()
            message(STATUS "[mcp] No explicit compiler set, using system default")
        endif()
    endif()
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/mcp/McpVersion.hpp" _mcp_version_hpp)
if(_mcp_version_hpp MATCHES "kSdkVersion[ \t]*=[ \t]*\"([0-9]+\\.[0-9]+\\.[0-9]+)\"")
    set(MCP_SDK_VERSION "${CMAKE_MATCH_1}")
    message(STATUS "[mcp] SDK version: ${MCP_SDK_VERSION}")
else()
    message(FATAL_ERROR "[mcp] Cannot parse kSdkVersion from include/mcp/McpVersion.hpp")
endif()

project(mcp-cpp-sdk VERSION ${MCP_SDK_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(MCP_BUILD_TESTS "Build unit/integration/conformance tests" OFF)
option(MCP_BUILD_EXAMPLES "Build example programs" OFF)
option(MCP_BUILD_CONFORMANCE "Build conformance server fixture (examples/conformance/server)" OFF)

function(mcp_enable_werror target)
    if(NOT MCP_WERROR)
        return()
    endif()
    if(MSVC)
        target_compile_options(${target} PUBLIC /WX)
    else()
        target_compile_options(${target} PUBLIC -Werror)
    endif()
    message(STATUS "[mcp] Werror: ${target}")
endfunction()

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_program(MCP_NINJA NAMES ninja)
if(MCP_NINJA AND CMAKE_GENERATOR MATCHES "Ninja")
    message(STATUS "[mcp] Ninja: ${MCP_NINJA}")
elseif(MCP_NINJA AND NOT CMAKE_GENERATOR MATCHES "Ninja")
    message(WARNING "[mcp] Ninja available (${MCP_NINJA}) but generator is '${CMAKE_GENERATOR}'. "
        "Use -G Ninja for faster builds.")
else()
    message(STATUS "[mcp] Ninja: not found (using ${CMAKE_GENERATOR})")
endif()

include(Platform)
include(BuildOptimization)
include(CompilerOptions)
include(Cache)
include(FetchDependencies)
include(Lto)

if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_GENERATOR MATCHES "Ninja")
    find_program(_lld_link NAMES lld-link)
    if(_lld_link)
        add_link_options("/lldlink")
        message(STATUS "[mcp] Linker: lld-link (${_lld_link})")
    endif()
endif()

add_library(mcp-core STATIC)
target_sources(mcp-core PRIVATE
    src/core/JsonValue.cpp
    src/core/JsonParser.cpp
    src/core/JsonRpc.cpp
    src/core/McpTypesTools.cpp
    src/core/McpTypesResults.cpp
    src/core/McpTypesNotifications.cpp
    src/core/McpTypesTasks.cpp
    src/core/McpTypesParams.cpp
    src/core/Content.cpp
    src/core/Capabilities.cpp
    src/detail/AtomicJsonFile.cpp)
target_include_directories(mcp-core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(mcp-core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(mcp-core PUBLIC Threads::Threads)
set_property(TARGET mcp-core PROPERTY UNITY_BUILD ${MCP_UNITY_ENABLED})
set_property(TARGET mcp-core PROPERTY UNITY_BUILD_BATCH_SIZE ${MCP_UNITY_BATCH})

add_library(mcp-transport STATIC)
target_sources(mcp-transport PRIVATE
    src/transport/StdioServerTransport.cpp
    src/transport/StdioClientTransport.cpp
    src/transport/InMemoryTransport.cpp
    src/transport/SseClientTransport.cpp
    src/transport/WebSocketClientTransport.cpp
    src/transport/detail/net/TlsSocket.cpp
    src/transport/detail/net/HttpClient.cpp
    src/transport/detail/net/WebSocketClient.cpp)
if(WIN32)
    target_sources(mcp-transport PRIVATE
        src/transport/detail/win32_platform.cpp
        src/transport/detail/net/TcpSocketWin32.cpp)
else()
    target_sources(mcp-transport PRIVATE
        src/transport/detail/posix_platform.cpp
        src/transport/detail/net/TcpSocketPosix.cpp)
endif()
target_link_libraries(mcp-transport PUBLIC mcp-core)
if(WIN32)
    target_link_libraries(mcp-transport PUBLIC ws2_32)
endif()
target_include_directories(mcp-transport PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_property(TARGET mcp-transport PROPERTY UNITY_BUILD OFF)

add_library(mcp-protocol STATIC)
target_sources(mcp-protocol PRIVATE
    src/protocol/WireCodec.cpp
    src/protocol/McpSessionHandler.cpp)
target_link_libraries(mcp-protocol PUBLIC mcp-transport)
target_include_directories(mcp-protocol PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_property(TARGET mcp-protocol PROPERTY UNITY_BUILD OFF)

add_library(mcp-server STATIC)
target_sources(mcp-server PRIVATE
    src/server/McpServer.cpp
    src/server/FileTaskStore.cpp)
target_link_libraries(mcp-server PUBLIC mcp-protocol)
target_include_directories(mcp-server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_property(TARGET mcp-server PROPERTY UNITY_BUILD ${MCP_UNITY_ENABLED})
set_property(TARGET mcp-server PROPERTY UNITY_BUILD_BATCH_SIZE ${MCP_UNITY_BATCH})

add_library(mcp-client STATIC)
target_sources(mcp-client PRIVATE
    src/client/McpClient.cpp
    src/client/auth/OAuthClientProvider.cpp
    src/client/FileTokenCache.cpp)
target_link_libraries(mcp-client PUBLIC mcp-protocol)
if(WIN32)
    target_link_libraries(mcp-client PUBLIC crypt32 bcrypt)
endif()
target_include_directories(mcp-client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(OpenSSL_FOUND)
    target_link_libraries(mcp-client PUBLIC OpenSSL::Crypto)
    target_link_libraries(mcp-transport PUBLIC OpenSSL::SSL)
    target_compile_definitions(mcp-client PRIVATE MCP_HAVE_OPENSSL)
    target_compile_definitions(mcp-transport PRIVATE MCP_HAVE_OPENSSL)
endif()
set_property(TARGET mcp-client PROPERTY UNITY_BUILD ${MCP_UNITY_ENABLED})
set_property(TARGET mcp-client PROPERTY UNITY_BUILD_BATCH_SIZE ${MCP_UNITY_BATCH})
set_property(TARGET mcp-client PROPERTY UNITY_BUILD_UNIQUE_ID ON)

add_library(mcp-http STATIC)
target_sources(mcp-http PRIVATE
    src/http/HttpServer.cpp
    src/http/HttpServerImpl.cpp
    src/http/EventStore.cpp
    src/http/StreamableHttpServerTransport.cpp)
target_sources(mcp-http PRIVATE src/http/StreamableHttpClientTransport.cpp)
target_sources(mcp-http PRIVATE src/http/FileEventStore.cpp)
if(WIN32)
    target_link_libraries(mcp-http PUBLIC winhttp)
endif()
target_link_libraries(mcp-http PUBLIC mcp-transport)
if(WIN32)
    target_link_libraries(mcp-http PUBLIC ws2_32)
endif()
target_include_directories(mcp-http PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_property(TARGET mcp-http PROPERTY UNITY_BUILD ${MCP_UNITY_ENABLED})
set_property(TARGET mcp-http PROPERTY UNITY_BUILD_BATCH_SIZE ${MCP_UNITY_BATCH})

foreach(_t mcp-core mcp-transport mcp-protocol mcp-server mcp-client mcp-http)
    mcp_enable_werror(${_t})
endforeach()

if(MCP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/framework)
    add_subdirectory(tests)
    foreach(_t
        mcp-test mcp-test-main mcp-framework-self-tests
        mcp-core-tests mcp-wire-codec-tests mcp-server-tests mcp-client-tests
        mcp-oauth-tests mcp-transport-tests mcp-net-tests mcp-http-tests
        mcp-message-filter-tests mcp-token-cache-tests mcp-task-store-tests
        mcp-event-store-tests mcp-streamable-http-tests mcp-websocket-tests
        mcp-integration-tests mcp-conformance-tests mcp-uri-template-tests)
        mcp_enable_werror(${_t})
    endforeach()
endif()

if(MCP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(MCP_BUILD_CONFORMANCE)
    add_subdirectory(examples/conformance/server)
endif()

file(WRITE "${CMAKE_BINARY_DIR}/build_config.txt"
    "version=${PROJECT_VERSION}\n"
    "build_type=${CMAKE_BUILD_TYPE}\n"
    "generator=${CMAKE_GENERATOR}\n"
    "compiler_id=${CMAKE_CXX_COMPILER_ID}\n"
    "compiler_launcher=${CMAKE_CXX_COMPILER_LAUNCHER}\n"
    "lto=${MCP_LTO}\n"
    "unity_enabled=${MCP_UNITY_ENABLED}\n"
    "unity_batch=${MCP_UNITY_BATCH}\n")
