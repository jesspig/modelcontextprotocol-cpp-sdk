# ====================================================================
# BuildOptimization — memory + CPU + file-count aware
#
# Ninja jobs:   min(memory_per_job, cpu_cores - 2)
# Unity batch:  min(memory_per_batch, cpu_cores / 2)
# Link jobs:    min(memory_per_link, 2) — linking is memory-heavy
#
# User overrides (cmake -D... or environment):
#   MCP_COMPILE_JOBS       — force compile parallelism
#   MCP_LINK_JOBS          — force link parallelism
#   MCP_UNITY_BATCH_SIZE   — force unity batch size (0 = auto)
#   MCP_MAX_COMPILE_MEM_MB — per-compile-job memory estimate (default 1500)
#   MCP_MAX_LINK_MEM_MB    — per-link-job memory estimate (default 4000)
#   MCP_UNITY_MEM_MB       — per-unity-file memory allowance (default 500)
# ====================================================================

# ── 1. Detect hardware ──
include(ProcessorCount)
ProcessorCount(_cpu)
if(_cpu LESS 1)
    set(_cpu 1)
endif()

cmake_host_system_information(RESULT _mem_mb QUERY TOTAL_PHYSICAL_MEMORY)
if(_mem_mb LESS 1)
    set(_mem_mb 1024)   # fallback: 1 GB
endif()

message(STATUS "[mcp] Hardware: ${_cpu} cores, ${_mem_mb} MB RAM")

# ── 2. User overrides with defaults ──
set(MCP_MAX_COMPILE_MEM_MB "1500" CACHE STRING
    "Estimated memory per compile job (MB)")
set(MCP_MAX_LINK_MEM_MB "4000" CACHE STRING
    "Estimated memory per link job (MB)")
set(MCP_UNITY_MEM_MB "500" CACHE STRING
    "Memory allowance per unity-batch file (MB)")

# ── 3. Compute compile pool (Ninja -j) ──
#     bounds: [1, cpu-2]
if(DEFINED CACHE{MCP_COMPILE_JOBS})
    set(_compile_jobs "${MCP_COMPILE_JOBS}")
elseif(DEFINED ENV{MCP_COMPILE_JOBS})
    set(_compile_jobs "$ENV{MCP_COMPILE_JOBS}")
else()
    math(EXPR _from_mem "${_mem_mb} / ${MCP_MAX_COMPILE_MEM_MB}")
    math(EXPR _from_cpu "${_cpu} - 2")
    if(_from_cpu LESS 1)
        set(_from_cpu 1)
    endif()
    if(_from_mem LESS _from_cpu)
        set(_compile_jobs ${_from_mem})
    else()
        set(_compile_jobs ${_from_cpu})
    endif()
    if(_compile_jobs LESS 1)
        set(_compile_jobs 1)
    endif()
endif()
set(CMAKE_BUILD_PARALLEL_LEVEL "${_compile_jobs}" CACHE STRING
    "Max parallel build jobs (auto)")

# ── 4. Compute link pool (Ninja pool, separate from compile) ──
#     linking needs more memory and blocks the linker, keep tight
if(DEFINED CACHE{MCP_LINK_JOBS})
    set(_link_jobs "${MCP_LINK_JOBS}")
elseif(DEFINED ENV{MCP_LINK_JOBS})
    set(_link_jobs "$ENV{MCP_LINK_JOBS}")
else()
    math(EXPR _from_mem_link "${_mem_mb} / ${MCP_MAX_LINK_MEM_MB}")
    if(_from_mem_link LESS 1)
        set(_from_mem_link 1)
    endif()
    # prefer memory bound over cpu bound for linking
    if(_from_mem_link LESS 2)
        set(_link_jobs ${_from_mem_link})
    else()
        set(_link_jobs 2)
    endif()
endif()

# ── 5. Create Ninja job pools ──
if(CMAKE_GENERATOR MATCHES "Ninja")
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS
        compile_pool=${_compile_jobs}
        link_pool=${_link_jobs})
    set(CMAKE_JOB_POOL_COMPILE compile_pool)
    set(CMAKE_JOB_POOL_LINK link_pool)
    message(STATUS "[mcp] Ninja pools: compile=${_compile_jobs}, link=${_link_jobs}")
else()
    message(STATUS "[mcp] Ninja jobs: ${_compile_jobs} (non-Ninja generator)")
endif()

# ── 6. Unity batch size ──
#     memory-aware:  each unity file should fit in MCP_UNITY_MEM_MB
#     cpu-aware:     cap at cpu/2 to keep parallelism
option(MCP_UNITY_BUILD "Enable unity (jumbo) build" ON)
set(MCP_UNITY_BATCH_SIZE "0" CACHE STRING
    "Unity batch count (0 = auto)")

set(MCP_UNITY_ENABLED OFF)
set(MCP_UNITY_BATCH 1)

if(MCP_UNITY_BUILD AND _cpu GREATER 1)
    set(MCP_UNITY_ENABLED ON)
    if(MCP_UNITY_BATCH_SIZE STREQUAL "0")
        math(EXPR _batch_from_mem "${_mem_mb} / ${MCP_UNITY_MEM_MB}")
        math(EXPR _batch_from_cpu "(${_cpu} + 1) / 2")
        if(_batch_from_cpu LESS 1)
            set(_batch_from_cpu 1)
        endif()
        if(_batch_from_mem LESS _batch_from_cpu)
            set(MCP_UNITY_BATCH ${_batch_from_mem})
        else()
            set(MCP_UNITY_BATCH ${_batch_from_cpu})
        endif()
        if(MCP_UNITY_BATCH LESS 2)
            set(MCP_UNITY_BATCH 2)   # minimum 2 per batch
        endif()
        message(STATUS "[mcp] Unity: batch=${MCP_UNITY_BATCH} "
            "(${_cpu} cores, ${_mem_mb} MB, ${MCP_UNITY_MEM_MB} MB/unit)")
    else()
        set(MCP_UNITY_BATCH ${MCP_UNITY_BATCH_SIZE})
        message(STATUS "[mcp] Unity: batch=${MCP_UNITY_BATCH} (user-specified)")
    endif()
else()
    message(STATUS "[mcp] Unity: disabled (${_cpu} core(s))")
endif()
