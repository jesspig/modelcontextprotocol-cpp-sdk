#!/usr/bin/env bash
set -o pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${MCP_INTEROP_OUT:-$ROOT/.tmp/interop}"
FIX="$ROOT/tests/interop"
CPP_DIR="${MCP_INTEROP_CPP_DIR:-$ROOT/build/debug/examples/conformance}"
NODE_VERSION="22.23.3"
PY_VERSION="$(python3 --version 2>&1 | awk '{print $2}')"

mkdir -p "$OUT"

export NVM_DIR="$HOME/.nvm"
if [ -s "$NVM_DIR/nvm.sh" ]; then
  . "$NVM_DIR/nvm.sh"
  nvm use "$NODE_VERSION" >/dev/null 2>&1 || nvm install "$NODE_VERSION" >/dev/null
fi
NODE_BIN="$(command -v node)"
PY="$FIX/py/.venv/bin/python"

wait_port() {
  local port=$1
  for _ in $(seq 1 160); do
    ss -ltn 2>/dev/null | grep -q ":$port " && return 0
    sleep 0.25
  done
  return 1
}

stop_pid() {
  if [ -f "$1" ]; then kill "$(cat "$1")" 2>/dev/null || true; rm -f "$1"; fi
}

prepare() {
  echo "=== prepare pinned SDK environments ==="
  echo "node=$(node -v) npm=$(npm -v) python=$PY_VERSION"
  cd "$FIX/ts" && npm install --no-audit --no-fund > "$OUT/npm-install.log" 2>&1
  echo "npm install exit=$?"
  python3 -m venv "$FIX/py/.venv" > "$OUT/venv.log" 2>&1
  "$PY" -m pip install -r "$FIX/py/requirements.txt" -q > "$OUT/pip-install.log" 2>&1
  echo "pip install exit=$?"
}

run_four_directions() {
  echo "=== direction 1: C++ server -> TS client ==="
  "$CPP_DIR/server/conformance-server" --port 32201 > "$OUT/d1-cpp-server.log" 2>&1 &
  echo $! > "$OUT/d1.pid"
  wait_port 32201
  (cd "$FIX/ts" && node client.mjs http://127.0.0.1:32201/mcp > "$OUT/d1-ts-client.log" 2>&1); echo $? > "$OUT/d1-ts-client.exit"
  stop_pid "$OUT/d1.pid"

  echo "=== direction 2: C++ server -> Python client ==="
  "$CPP_DIR/server/conformance-server" --port 32202 > "$OUT/d2-cpp-server.log" 2>&1 &
  echo $! > "$OUT/d2.pid"
  wait_port 32202
  "$PY" "$FIX/py/client.py" http://127.0.0.1:32202/mcp > "$OUT/d2-python-client.log" 2>&1; echo $? > "$OUT/d2-python-client.exit"
  stop_pid "$OUT/d2.pid"

  echo "=== direction 3: TS server -> C++ client ==="
  (cd "$FIX/ts" && node server.mjs 32210 > "$OUT/d3-ts-server.log" 2>&1) &
  echo $! > "$OUT/d3.pid"
  wait_port 32210
  for s in initialize tools_call http-standard-headers http-invalid-tool-headers; do
    MCP_CONFORMANCE_FORCE_LEGACY=1 MCP_CONFORMANCE_SCENARIO="$s" \
      "$CPP_DIR/client/conformance-client" http://127.0.0.1:32210/mcp > "$OUT/d3-cpp-$s.log" 2>&1
    echo $? > "$OUT/d3-cpp-$s.exit"
  done
  stop_pid "$OUT/d3.pid"

  echo "=== direction 4: Python server -> C++ client ==="
  "$PY" "$FIX/py/server.py" 32211 > "$OUT/d4-python-server.log" 2>&1 &
  echo $! > "$OUT/d4.pid"
  wait_port 32211
  for s in initialize tools_call http-invalid-tool-headers; do
    MCP_CONFORMANCE_SCENARIO="$s" "$CPP_DIR/client/conformance-client" \
      http://127.0.0.1:32211/mcp > "$OUT/d4-cpp-$s.log" 2>&1
    echo $? > "$OUT/d4-cpp-$s.exit"
  done
  MCP_CONFORMANCE_PROTOCOL_VERSION=2026-07-28 MCP_CONFORMANCE_SCENARIO=http-standard-headers \
    "$CPP_DIR/client/conformance-client" http://127.0.0.1:32211/mcp > "$OUT/d4-cpp-http-standard-headers.log" 2>&1
  echo $? > "$OUT/d4-cpp-http-standard-headers.exit"
  stop_pid "$OUT/d4.pid"
}

run_tasks() {
  echo "=== tasks (official TS client raw JSON-RPC) ==="
  "$CPP_DIR/server/conformance-server" --port 32231 --stateless > "$OUT/tasks-cpp-server.log" 2>&1 &
  echo $! > "$OUT/tasks.pid"
  wait_port 32231
  (cd "$FIX/ts" && node tasks-raw.mjs http://127.0.0.1:32231/mcp > "$OUT/tasks-ts-client.log" 2>&1); echo $? > "$OUT/tasks-ts-client.exit"
  stop_pid "$OUT/tasks.pid"
}

run_oauth() {
  echo "=== oauth (local AS, synthetic token) ==="
  "$PY" "$FIX/py/local_as.py" 32300 http://127.0.0.1:32240/mcp "$OUT/as-ts.log" > "$OUT/as-ts-stdout.log" 2>&1 &
  echo $! > "$OUT/as-ts.pid"
  "$CPP_DIR/server/conformance-server" --port 32240 --oauth --as-url http://127.0.0.1:32300 > "$OUT/oauth-ts-cpp-server.log" 2>&1 &
  echo $! > "$OUT/oauth-ts.pid"
  wait_port 32300 && wait_port 32240
  (cd "$FIX/ts" && node oauth-client.mjs http://127.0.0.1:32240/mcp 32399 > "$OUT/oauth-ts-client.log" 2>&1); echo $? > "$OUT/oauth-ts-client.exit"
  stop_pid "$OUT/oauth-ts.pid"; stop_pid "$OUT/as-ts.pid"

  "$PY" "$FIX/py/local_as.py" 32301 http://127.0.0.1:32241/mcp "$OUT/as-py.log" > "$OUT/as-py-stdout.log" 2>&1 &
  echo $! > "$OUT/as-py.pid"
  "$CPP_DIR/server/conformance-server" --port 32241 --oauth --as-url http://127.0.0.1:32301 > "$OUT/oauth-py-cpp-server.log" 2>&1 &
  echo $! > "$OUT/oauth-py.pid"
  wait_port 32301 && wait_port 32241
  "$PY" "$FIX/py/oauth_client.py" http://127.0.0.1:32241/mcp 32398 > "$OUT/oauth-python-client.log" 2>&1; echo $? > "$OUT/oauth-python-client.exit"
  stop_pid "$OUT/oauth-py.pid"; stop_pid "$OUT/as-py.pid"
}

run_http() {
  echo "=== http wire scenarios ==="
  "$CPP_DIR/server/conformance-server" --port 32250 > "$OUT/http-cpp-server.log" 2>&1 &
  echo $! > "$OUT/http-cpp.pid"
  wait_port 32250
  MCP_JSON_RESPONSE=1 "$PY" "$FIX/py/server.py" 32251 > "$OUT/http-py-server.log" 2>&1 &
  echo $! > "$OUT/http-py.pid"
  wait_port 32251
  (cd "$FIX/ts" && node http.mjs http://127.0.0.1:32250 http://127.0.0.1:32251 > "$OUT/http-ts-client.log" 2>&1); echo $? > "$OUT/http-ts-client.exit"
  stop_pid "$OUT/http-py.pid"; stop_pid "$OUT/http-cpp.pid"

  "$PY" "$FIX/py/blackhole.py" 32252 > "$OUT/http-blackhole.log" 2>&1 &
  echo $! > "$OUT/http-blackhole.pid"
  wait_port 32252
  (cd "$FIX/ts" && node http-timeout.mjs http://127.0.0.1:32252/mcp > "$OUT/http-timeout.log" 2>&1); echo $? > "$OUT/http-timeout.exit"
  stop_pid "$OUT/http-blackhole.pid"
}

case "${1:-all}" in
  prepare) prepare ;;
  four-directions) run_four_directions ;;
  tasks) run_tasks ;;
  oauth) run_oauth ;;
  http) run_http ;;
  all) prepare; run_four_directions; run_tasks; run_oauth; run_http ;;
  *) echo "usage: $0 {prepare|four-directions|tasks|oauth|http|all}" >&2; exit 2 ;;
esac

echo "=== done; logs in $OUT ==="
