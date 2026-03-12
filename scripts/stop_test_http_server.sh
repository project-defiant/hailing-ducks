#!/usr/bin/env bash
# Stop the local Python HTTP server started by start_test_http_server.sh.
set -euo pipefail

PID_FILE=/tmp/hailing_ducks_http_server.pid

if [ -f "$PID_FILE" ]; then
    kill "$(cat "$PID_FILE")" 2>/dev/null && echo "HTTP test server stopped."
    rm -f "$PID_FILE"
else
    echo "No PID file found at $PID_FILE — server may not be running."
fi
