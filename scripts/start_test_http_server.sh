#!/usr/bin/env bash
# Start a local Python HTTP server serving the repo root.
# Used by `make test_http` to provide an http:// endpoint for the httpfs integration tests.
set -euo pipefail

PORT=${HTTP_TEST_PORT:-18642}
REPO_ROOT="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"

cd "$REPO_ROOT"
python3 -m http.server "$PORT" --bind 127.0.0.1 >/dev/null 2>&1 &
echo $! > /tmp/hailing_ducks_http_server.pid
echo "HTTP test server started on port $PORT (PID $(cat /tmp/hailing_ducks_http_server.pid))"
