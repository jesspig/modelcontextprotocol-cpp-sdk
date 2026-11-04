if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if(WIN32)
        add_compile_options(
            /utf-8
            /bigobj
            /W4
            /wd4100
            /wd4324
            /wd4244
            /wd4267
            /EHsc
        )
        add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
        add_compile_definitions(_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS)
        add_compile_definitions(_WIN32_WINNT=0x0A00)
        add_compile_definitions(NOMINMAX NOGDI)
        message(STATUS "[mcp] clang-cl flags applied")
    else()
        add_compile_options(
            -Wall -Wextra -Wpedantic
            -Wno-unused-parameter
        )
        if(NOT MCP_IS_CI AND NOT APPLE)
            add_compile_options(-march=native)
        endif()
        message(STATUS "[mcp] Clang flags applied")
    endif()

elseif(MSVC)
    add_compile_options(
        /utf-8
        /bigobj
        /W4
        /wd4100
        /wd4324
        /wd4244
        /wd4267
        /EHsc
    )
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS
        _WIN32_WINNT=0x0A00
        NOMINMAX
        NOGDI
    )
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded CACHE INTERNAL "")
    message(STATUS "[mcp] MSVC flags applied")

elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
    )
    if(NOT MCP_IS_CI AND NOT APPLE)
        add_compile_options(-march=native)
    endif()
    message(STATUS "[mcp] GCC flags applied")

else()
    message(WARNING "[mcp] Unknown compiler: ${CMAKE_CXX_COMPILER_ID}")
endif()

set(MCP_SANITIZE "off" CACHE STRING
    "Runtime sanitizer: off, address (ASan+UBSan) or thread (TSan)")
set_property(CACHE MCP_SANITIZE PROPERTY STRINGS off address thread)

if(NOT MCP_SANITIZE STREQUAL "off")
    if(MSVC)
        message(WARNING "[mcp] sanitize: '${MCP_SANITIZE}' unsupported on MSVC, ignored")
    elseif(MCP_SANITIZE STREQUAL "address")
        add_compile_options(
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all
        )
        add_link_options(-fsanitize=address,undefined)
        message(STATUS "[mcp] sanitize: address+undefined (ASan+UBSan)")
    elseif(MCP_SANITIZE STREQUAL "thread")
        add_compile_options(
            -fsanitize=thread
            -fno-omit-frame-pointer
        )
        add_link_options(-fsanitize=thread)
        message(STATUS "[mcp] sanitize: thread (TSan)")
    else()
        message(FATAL_ERROR
            "[mcp] MCP_SANITIZE must be one of: off, address, thread (got '${MCP_SANITIZE}')")
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    add_compile_definitions($<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(
        $<$<CONFIG:Release>:-D_FORTIFY_SOURCE=3>
        $<$<CONFIG:Release>:-fstack-protector-strong>
    )
    add_link_options($<$<CONFIG:Release>:-fstack-protector-strong>)
endif()

include(FindThreads)
find_package(Threads REQUIRED)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
