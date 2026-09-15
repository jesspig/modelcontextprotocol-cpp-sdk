#!/bin/bash
# 本地复现 .github/workflows/conformance.yml 的官方 conformance 驱动行为。
# 用法: scripts/run-conformance.sh [server|client] [透传给 conformance CLI 的额外参数...]
# 环境变量: PORT (默认 3010, 仅 server 模式), BUILD_PRESET (默认 debug)
#
# 前置: 先完成配置与构建，例如
#   cmake --preset debug -DMCP_BUILD_CONFORMANCE=ON -DMCP_BUILD_EXAMPLES=ON
#   cmake --build --preset debug --target conformance-server conformance-client
#
# 退出码语义由 referee 的 expected-failures 机制保证：
# 基线外失败 -> 非 0（真回归）；过期基线条目 -> 非 0（须删条目）。

set -e

MODE="${1:-server}"
shift || true

PORT="${PORT:-3010}"
BUILD_PRESET="${BUILD_PRESET:-debug}"
SPEC_VERSION="2025-11-25"
BASELINE="tests/conformance/baseline.yaml"
CONFORMANCE_PKG="@modelcontextprotocol/conformance@0.2.0-alpha.11"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

BIN_DIR="build/${BUILD_PRESET}/examples/conformance"
# Windows 构建产物带 .exe 后缀，Linux/macOS 无后缀。
resolve_binary() {
    if [ -f "${BIN_DIR}/$1.exe" ]; then
        echo "${BIN_DIR}/$1.exe"
    else
        echo "${BIN_DIR}/$1"
    fi
}

run_referee() {
    npx -y "${CONFORMANCE_PKG}" "$@"
}

case "${MODE}" in
server)
    SERVER_BIN="$(resolve_binary conformance-server)"
    if [ ! -f "${SERVER_BIN}" ]; then
        echo "Error: ${SERVER_BIN} not found. Build first (see header comments)."
        exit 1
    fi
    SERVER_URL="http://localhost:${PORT}/mcp"

    # 端口被占即拒跑：就绪探测无法区分本进程与残留监听者，残留进程会让
    # conformance 对旧代码跑出假结论（对齐 TS run-server-conformance.sh）。
    if (: > "/dev/tcp/localhost/${PORT}") 2>/dev/null; then
        echo "Error: port ${PORT} is already in use."
        echo "Stop the stale process first or set PORT to a free port."
        exit 1
    fi

    echo "Starting conformance test server on port ${PORT}..."
    "${SERVER_BIN}" --port "${PORT}" &
    SERVER_PID=$!

    cleanup() {
        echo "Stopping server (PID: ${SERVER_PID})..."
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    }
    trap cleanup EXIT

    echo "Waiting for server to be ready..."
    MAX_RETRIES=30
    RETRY_COUNT=0
    # 选 initialize：当前单 leg server 为 legacy 2025-11-25 有状态模式，必有 JSON 应答；POST 探测不会像 GET 一样建立 SSE 长流导致 --max-time 误超时。
    probe_ready() {
        curl -s --max-time 2 -X POST "${SERVER_URL}" \
            -H "Content-Type: application/json" \
            -H "Accept: application/json, text/event-stream" \
            -d '{"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"probe","version":"1.0"}}}' \
            > /dev/null 2>&1
    }
    while ! probe_ready; do
        if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
            echo "Server process exited unexpectedly"
            exit 1
        fi
        RETRY_COUNT=$((RETRY_COUNT + 1))
        if [ "${RETRY_COUNT}" -ge "${MAX_RETRIES}" ]; then
            echo "Server failed to start after ${MAX_RETRIES} attempts"
            exit 1
        fi
        sleep 0.5
    done

    echo "Server is ready. Running conformance tests..."
    run_referee server --url "${SERVER_URL}" \
        --spec-version "${SPEC_VERSION}" \
        --expected-failures "${BASELINE}" \
        "$@"
    ;;
client)
    CLIENT_BIN="$(resolve_binary conformance-client)"
    if [ ! -f "${CLIENT_BIN}" ]; then
        echo "Error: ${CLIENT_BIN} not found. Build first (see header comments)."
        exit 1
    fi
    # client 模式由 referee 自带 per-scenario test server 驱动 C++ client。
    run_referee client --command "${CLIENT_BIN}" \
        --suite core \
        --spec-version "${SPEC_VERSION}" \
        --expected-failures "${BASELINE}" \
        "$@"
    ;;
*)
    echo "Usage: $0 [server|client] [extra conformance args...]"
    exit 1
    ;;
esac

echo "Conformance tests completed."
