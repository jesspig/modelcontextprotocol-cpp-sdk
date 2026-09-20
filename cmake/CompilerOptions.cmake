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

include(FindThreads)
find_package(Threads REQUIRED)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
