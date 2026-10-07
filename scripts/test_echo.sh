#!/usr/bin/env bash
# Sends the 10 sample echo requests one after another (Parts 1-3).
# Usage: scripts/test_echo.sh [port]
set -u
PORT="${1:-8080}"
CLIENT="$(dirname "$0")/../bin/echo_client"

if [ ! -x "$CLIENT" ]; then
    echo "bin/echo_client not found - run 'make' first." >&2
    exit 1
fi

LONG_MESSAGE="$(printf 'A%.0s' $(seq 1 3000))"      # > 1024 bytes: needs several read() calls
EXACT_BUFFER="$(printf 'B%.0s' $(seq 1 1024))"      # exactly one buffer

MESSAGES=(
    "Message 1: Hello, server!"
    "Message 2: a second, slightly longer sentence with spaces and punctuation."
    ""
    "$LONG_MESSAGE"
    $'Message 5: line one\nline two\nline three'
    "Message 6: special chars !@#\$%^&*()[]{}<>?/\\|~"
    "Message 7: unicode - café, naïve, 你好, 🚀"
    "Message 8: 1234567890"
    "$EXACT_BUFFER"
    "Message 10: goodbye!"
)
LABELS=(
    "plain message" "longer sentence" "empty string" "3000 bytes (bigger than the 1024-byte buffer)"
    "multi-line" "special characters" "unicode (multi-byte)" "digits"
    "exactly 1024 bytes" "final message"
)

failed=0
for i in "${!MESSAGES[@]}"; do
    echo "--- Request $((i + 1))/10: ${LABELS[$i]}  [$(date '+%H:%M:%S')]"
    "$CLIENT" localhost "$PORT" "${MESSAGES[$i]}" || failed=$((failed + 1))
done

echo
if [ "$failed" -eq 0 ]; then
    echo "All 10 requests echoed back correctly."
else
    echo "$failed of 10 requests FAILED."
    exit 1
fi
