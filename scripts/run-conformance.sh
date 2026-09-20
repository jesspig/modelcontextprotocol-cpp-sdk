#!/bin/bash

set -e

MODE="${1:-server}"
shift || true

PORT="${PORT:-3010}"
BUILD_PRESET="${BUILD_PRESET:-debug}"
CONFORMANCE_SPEC_VERSION="${CONFORMANCE_SPEC_VERSION:-2025-11-25}"
BASELINE="tests/conformance/baseline.yaml"
CONFORMANCE_PKG="@modelcontextprotocol/conformance@0.2.0-alpha.11"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

BIN_DIR="build/${BUILD_PRESET}/examples/conformance"
resolve_binary() {
    local name="$1"
    local target_dir="${BIN_DIR}/${name}"
    if [ -f "${target_dir}/${name}.exe" ]; then
        echo "${target_dir}/${name}.exe"
        return 0
    fi
    if [ -f "${target_dir}/${name}" ]; then
        echo "${target_dir}/${name}"
        return 0
    fi
    if [ -f "${BIN_DIR}/${name}.exe" ]; then
        echo "Warning: ${target_dir} not found; falling back to legacy layout ${BIN_DIR}" >&2
        echo "${BIN_DIR}/${name}.exe"
        return 0
    fi
    if [ -f "${BIN_DIR}/${name}" ]; then
        echo "Warning: ${target_dir} not found; falling back to legacy layout ${BIN_DIR}" >&2
        echo "${BIN_DIR}/${name}"
        return 0
    fi
    echo "${target_dir}/${name}"
}

run_referee() {
    npx -y "${CONFORMANCE_PKG}" "$@"
}

case "${MODE}" in
server)
    SERVER_BIN="$(resolve_binary conformance-server)"
    if [ ! -f "${SERVER_BIN}" ]; then
        echo "Error: ${SERVER_BIN} not found. Build first (see AGENTS.md)."
        exit 1
    fi
    SERVER_URL="http://localhost:${PORT}/mcp"

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
    probe_ready() {
        curl -s --max-time 2 -X POST "${SERVER_URL}" \
            -H "Content-Type: application/json" \
            -H "Accept: application/json, text/event-stream" \
            -d '{"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":"'${CONFORMANCE_SPEC_VERSION}'","capabilities":{},"clientInfo":{"name":"probe","version":"1.0"}}}' \
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
        --spec-version "${CONFORMANCE_SPEC_VERSION}" \
        --expected-failures "${BASELINE}" \
        "$@"
    ;;
client)
    CLIENT_BIN="$(resolve_binary conformance-client)"
    if [ ! -f "${CLIENT_BIN}" ]; then
        echo "Error: ${CLIENT_BIN} not found. Build first (see AGENTS.md)."
        exit 1
    fi
    run_referee client --command "${CLIENT_BIN}" \
        --suite core \
        --spec-version "${CONFORMANCE_SPEC_VERSION}" \
        --expected-failures "${BASELINE}" \
        "$@"
    ;;
*)
    echo "Usage: $0 [server|client] [extra conformance args...]"
    exit 1
    ;;
esac

echo "Conformance tests completed."
