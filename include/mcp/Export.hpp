#pragma once

#if defined(_MSC_VER)
#  define MCP_EXPORT __declspec(dllexport)
#  define MCP_IMPORT __declspec(dllimport)
#elif defined(__GNUC__) || defined(__clang__)
#  define MCP_EXPORT __attribute__((visibility("default")))
#  define MCP_IMPORT __attribute__((visibility("default")))
#else
#  define MCP_EXPORT
#  define MCP_IMPORT
#  define MCP_API
#endif

#if defined(MCP_STATIC) || !defined(MCP_BUILD_SHARED)
#  define MCP_API
#elif defined(MCP_BUILD_CORE)
#  define MCP_API MCP_EXPORT
#elif defined(MCP_USE_CORE)
#  define MCP_API MCP_IMPORT
#else
#  define MCP_API
#endif
