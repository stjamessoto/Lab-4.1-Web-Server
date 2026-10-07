#!/usr/bin/env bash
# Runs the 10 sample HTTP requests plus the path-traversal attacks (Part 4)
# and checks each status code.
# Usage: scripts/test_http.sh [port]
set -u
PORT="${1:-8080}"
BASE="http://localhost:$PORT"

if ! curl -s -o /dev/null --max-time 2 "$BASE/"; then
    echo "No server answering on $BASE - start it with: ./bin/http_server $PORT" >&2
    exit 1
fi

failed=0
# --path-as-is stops curl from tidying "/../" away before sending it.
check() {
    local expected="$1" path="$2" note="$3"
    local code type size
    IFS='|' read -r code type size < <(curl -s -o /dev/null --path-as-is --max-time 5 \
        -w '%{http_code}|%{content_type}|%{size_download}' "$BASE$path")
    local verdict="ok"
    if [ "$code" != "$expected" ]; then
        verdict="FAIL (expected $expected)"
        failed=$((failed + 1))
    fi
    printf '%-40s %-4s %-26s %6s B  %-4s %s\n' "$path" "$code" "$type" "$size" "$verdict" "$note"
}

echo "=== 10 sample requests  [$(date '+%Y-%m-%d %H:%M:%S')] ==="
check 200 "/"                             "serves index.html"
check 200 "/index.html"                   ""
check 200 "/style.css"                    "text/css"
check 200 "/logo.png"                     "image/png"
check 200 "/subpage.html"                 ""
check 404 "/missing.txt"                  "does not exist"
check 404 "/favicon.ico"                  "does not exist"
check 404 "/.."                           "security test"
check 200 "/?test=1"                      "query string"
check 404 "/very/long/path/to/file.html"  "does not exist"

echo
echo "=== Path traversal attacks (all must be 404) ==="
check 404 "/../../../etc/passwd"                    "plain ../"
check 404 "/../%2e%2e/%2e%2e/etc/passwd"            "encoded dots (from the lab sheet)"
check 404 "/%2e%2e/%2e%2e/%2e%2e/etc/passwd"        "encoded dots"
check 404 "/..%2f..%2f..%2fetc%2fpasswd"            "encoded slashes"
check 404 "/subdir/../../../etc/passwd"             "starts inside the root"
check 400 "/%00"                                    "NUL byte"
check 400 "/%zz"                                    "malformed escape"

echo
if [ "$failed" -eq 0 ]; then
    echo "All checks passed."
else
    echo "$failed check(s) FAILED."
    exit 1
fi
