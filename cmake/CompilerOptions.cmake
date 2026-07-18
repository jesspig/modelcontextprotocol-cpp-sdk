# ====================================================================
# Compiler-specific flags — auto-detected from CMAKE_CXX_COMPILER_ID
# 优先级: Clang(clang-cl) > MSVC(cl.exe) > GCC > fallback
# ====================================================================

# ── Clang (including clang-cl on Windows) ──
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if(WIN32)
        # clang-cl on Windows — MSVC-compatible flags
        add_compile_options(
            /utf-8
            /bigobj
            /W4
            /wd4324
            /wd4244
            /wd4267
            /EHsc
        )
        add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
        add_compile_definitions(_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS)
        add_compile_definitions(_WIN32_WINNT=0x0A00)
        message(STATUS "[mcp] clang-cl flags applied")
    else()
        add_compile_options(
            -Wall -Wextra -Wpedantic
            -Wno-unused-parameter
        )
        if(NOT MCP_IS_CI)
            add_compile_options(-march=native)
        endif()
        message(STATUS "[mcp] Clang flags applied")
    endif()

# ── MSVC (cl.exe only) ──
elseif(MSVC)
    add_compile_options(
        /utf-8
        /bigobj
        /W4
        /wd4324
        /wd4244
        /wd4267
        /EHsc
    )
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS
        _WIN32_WINNT=0x0A00
    )
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded CACHE INTERNAL "")
    message(STATUS "[mcp] MSVC flags applied")

# ── GNU GCC ──
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
    )
    if(NOT MCP_IS_CI)
        add_compile_options(-march=native)
    endif()
    message(STATUS "[mcp] GCC flags applied")

else()
    message(WARNING "[mcp] Unknown compiler: ${CMAKE_CXX_COMPILER_ID}")
endif()

# ── Threads — required for std::thread on macOS/Clang (Unity builds) ──
include(FindThreads)
find_package(Threads REQUIRED)

# ── ASIO ──
add_compile_definitions(ASIO_STANDALONE ASIO_NO_DEPRECATED)

# ── PIC ──
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
