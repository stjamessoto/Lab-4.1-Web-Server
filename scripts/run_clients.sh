#!/usr/bin/env bash
# Launches N echo clients at the same moment and reports the total time.
# Compare the result between Part 1, Part 2 and Part 3.
# Usage: scripts/run_clients.sh [port] [count]
set -u
PORT="${1:-8080}"
COUNT="${2:-10}"
CLIENT="$(dirname "$0")/../bin/echo_client"

if [ ! -x "$CLIENT" ]; then
    echo "bin/echo_client not found - run 'make' first." >&2
    exit 1
fi

echo "Launching $COUNT clients in parallel against port $PORT  [$(date '+%H:%M:%S')]"
start=$(date +%s%N)

pids=()
for i in $(seq 1 "$COUNT"); do
    # Vary the message size: every third client sends a large message.
    if [ $((i % 3)) -eq 0 ]; then
        message="Client $i: $(printf 'x%.0s' $(seq 1 $((i * 200))))"
    else
        message="Client $i says hello"
    fi
    "$CLIENT" localhost "$PORT" "$message" | sed "s/^/[client $i] /" &
    pids+=($!)
done

failed=0
for pid in "${pids[@]}"; do
    wait "$pid" || failed=$((failed + 1))
done

elapsed_ms=$(( ($(date +%s%N) - start) / 1000000 ))
echo
printf 'All %s clients finished in %d.%03d seconds (%s failed)\n' "$COUNT" $((elapsed_ms / 1000)) $((elapsed_ms % 1000)) "$failed"
