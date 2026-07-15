#pragma once

// ── MCP_API — Symbol visibility / DLL export control ──
// Use this macro on all public class declarations:
//   class MCP_API MyClass { ... };
//
// Currently all libraries are STATIC, so this is a no-op.
// When switching to shared libraries, define MCP_BUILD_SHARED
// and MCP_BUILD_{LIBRARY_NAME} for the exporting library.

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

// For static builds (current), MCP_API is a no-op
#if defined(MCP_STATIC) || !defined(MCP_BUILD_SHARED)
#  define MCP_API
#elif defined(MCP_BUILD_CORE)
#  define MCP_API MCP_EXPORT
#elif defined(MCP_USE_CORE)
#  define MCP_API MCP_IMPORT
#else
#  define MCP_API
#endif