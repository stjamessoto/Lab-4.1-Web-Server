# Lab 4.1 – Client-Server Applications in C++

From a single-threaded echo server to a thread-pool HTTP web server.

| Part | Program | What it shows |
|------|---------|---------------|
| 1 | `echo_server` + `echo_client` | One client at a time; the others queue |
| 2 | `multi_threaded_server` | One pthread per client, mutex-protected connection counter |
| 3 | `thread_pool_server` | 10 fixed workers, task queue, graceful Ctrl+C shutdown |
| 4 | `http_server` | Static files from `./www`, correct `Content-Type`, path-traversal defence |

## Quick start

```bash
make                     # builds everything into ./bin
./bin/http_server        # then open http://localhost:8080/ in a browser
```

Requirements: Linux, macOS or WSL with `g++` (C++17), `make`, `curl`, and `telnet` or `nc`.
On Ubuntu/WSL: `sudo apt install build-essential curl telnet`.

No `make`? Each program is one file plus the shared header:

```bash
g++ -std=c++17 -pthread -o server src/echo_server.cpp
```

## What to submit

| Item | Where | Status |
|------|-------|--------|
| Source code | [`src/`](src/) | Done |
| Web files for Part 4 | [`www/`](www/) | Done |
| Test scripts | [`scripts/`](scripts/) | Done |
| Screenshots | [`screenshots/`](screenshots/) | **You** – checklist below |
| Report | [`report/report.tex`](report/report.tex) → `report/report.pdf` | **You** – written up; add screenshots and fill in the red fields |

When the screenshots and report are in place, `make zip` rebuilds the report and creates
`Lab4.1_submission.zip` with all of the above.

## Project layout

```
src/
  common.hpp                 shared helpers: logging, socket setup, error handling
  echo_server.cpp            Part 1
  echo_client.cpp            test client for Parts 1–3
  multi_threaded_server.cpp  Part 2
  thread_pool_server.cpp     Part 3
  http_server.cpp            Part 4
www/                         index.html, style.css, subpage.html, logo.png
scripts/
  test_echo.sh               the 10 sample echo requests
  run_clients.sh             N clients in parallel, prints total time
  test_http.sh               the 10 sample HTTP requests + traversal attacks
screenshots/                 put your screenshots here
report/report.tex            LaTeX report; pulls screenshots in by file name
```

## Running each part

Every server takes the port as its first argument (default `8080`). Run servers from the project root.
Stop a server with **Ctrl+C**. Use two terminals: server in one, tests in the other.

The echo servers accept an optional second argument, `delay_ms`, which simulates slow work per
client. Without it everything finishes in milliseconds and you cannot see queuing or concurrency.

### Part 1 – single-threaded echo server

```bash
./bin/echo_server 8080                              # terminal 1
./bin/echo_client localhost 8080 "Hello, server!"   # terminal 2
scripts/test_echo.sh 8080                           # all 10 sample requests
```

Queuing test (restart the server with a 2 s delay):

```bash
./bin/echo_server 8080 2000        # terminal 1
scripts/run_clients.sh 8080 3      # terminal 2 – takes ~6 s, one client at a time
```

### Part 2 – one thread per client

```bash
./bin/multi_threaded_server 8080 500    # terminal 1
time scripts/run_clients.sh 8080 10     # terminal 2
```

Compare with Part 1 using the same delay (`./bin/echo_server 8080 500`).

### Part 3 – thread pool

```bash
./bin/thread_pool_server 8080 500     # terminal 1
scripts/run_clients.sh 8080 20        # terminal 2 – 20 clients, 10 workers
```

The log prints the queue size on every push and pop. Press Ctrl+C while clients are still
queued to see the server drain the queue before exiting.

### Part 4 – HTTP server

```bash
./bin/http_server 8080                  # terminal 1
scripts/test_http.sh 8080               # terminal 2 – all sample requests, pass/fail table
curl -v http://localhost:8080/          # a single request in detail
```

Browser: <http://localhost:8080/>

> **curl and `..`** – curl silently removes `../` from URLs before sending them. Add
> `--path-as-is` to send the attack exactly as written:
> `curl -v --path-as-is http://localhost:8080/../../../etc/passwd`

## Measured results

Measured on the development machine with a 500 ms simulated delay per client:

| Server | Clients | Total time |
|--------|---------|-----------|
| Part 1 – single-threaded | 10 | 5.01 s |
| Part 2 – thread per client | 10 | 0.51 s |
| Part 3 – pool of 10 | 20 | 1.01 s |

Re-run these yourself for the report; your numbers will be close but not identical.

## Lab requirements and where they are implemented

**All parts**

- Error handling: invalid/out-of-range ports, port already in use, unresolvable host, refused
  connection, partial writes, interrupted calls, clients that disconnect early (`common.hpp`).
- Logging: every event has a millisecond timestamp; the logger is mutex-protected so lines from
  different threads never mix (`lab::log`).

**Part 1**

- Multi-line and oversized messages: the server reads in a loop until EOF rather than doing a
  single 1024-byte read (`lab::echo_until_eof`). The client half-closes the socket
  (`shutdown(SHUT_WR)`) to signal the end of its message.
- The lab's client used `inet_pton`, which cannot resolve the name `localhost`. The client here
  uses `getaddrinfo`, so `./bin/echo_client localhost 8080 ...` works as the lab sheet shows.

**Part 2**

- Shared connection counter and active-thread counter protected by a `pthread_mutex_t`.
- Thread limit (`MAX_THREADS = 200`): clients beyond it are refused instead of crashing the server.

**Part 3**

- Queue size logged on every push and pop.
- SIGINT/SIGTERM handler: stop accepting, drain the queue, join the workers, exit.

**Part 4**

- `parse_request`: reads the complete header block, validates the request line, parses headers,
  decodes the URL (`%20` → space), parses query parameters, reads POST bodies.
- `get_file_content`: binary-safe file reads; `/` and directories map to `index.html`.
- `Content-Type` chosen from the file extension (`get_content_type`).
- Methods: `GET`, `HEAD`, `POST` (echoes the body); anything else gets `405`.
- Other statuses: `400` malformed request, `404` missing file, `408` slow client, `413` body too
  large, `431` headers too large.
- Full request (request line, headers, query parameters, body) written to the log.

**Cybersecurity challenge – path traversal** (`resolve_path` in `http_server.cpp`)

1. The URL is percent-decoded once, before any check, so `%2e%2e` becomes `..`.
2. `std::filesystem::weakly_canonical` resolves `.`, `..` and symlinks to a real absolute path.
3. That path must still be inside the canonical web root (compared component by component).
   Otherwise the request is logged as `SECURITY: blocked path traversal attempt` and answered
   with `404`.

Malformed escapes and `%00` are rejected with `400`. A symlink inside `www/` pointing outside
it is also blocked, because the check runs on the resolved path rather than on the URL text.

## Screenshots and report

The report is [`report/report.tex`](report/report.tex). It looks in `screenshots/` for the file
names below (`.png`, `.jpg` or `.jpeg`) and places each one in the right section. A screenshot
that is not there yet appears as a red "Missing screenshot" box.

```bash
make report      # builds report/report.pdf and lists the screenshots still missing
```

Building needs LaTeX: `sudo apt install texlive-latex-recommended texlive-latex-extra`.

Also fill in the red `[ ... ]` fields in the PDF: search `report.tex` for `\fillin` (name,
environment, the measured times and thread counts, and the closing paragraph).

**How to capture**

- Put Terminal 1 (server) on the left and Terminal 2 (client) on the right.
- Run `clear` in both before each test so only that test is on screen.
- "Both" below means one screenshot covering both terminals.
- Windows + WSL: `Win+Shift+S` to capture; `explorer.exe screenshots` opens the folder to save into.

| Save as (in `screenshots/`) | Capture | Must be visible |
|---|---|---|
| `p1_requests` | Both | The `test_echo.sh` output down to "All 10 requests echoed back correctly", and the server log |
| `p1_requests_2` (optional) | Both | The rest, if it did not fit in one screenshot |
| `p1_telnet` | Both | The lines you typed in telnet echoed back; "received" lines in the server log |
| `p1_queuing` | Both | Server log timestamps about 2 s apart; "All 3 clients finished in 6.0 seconds" |
| `p1_errors` | Both | The two usage errors (Terminal 1) and "Connection refused" (Terminal 2) |
| `p2_concurrent` | Both | Interleaved client lines and the "total connections / active threads" counters |
| `p2_timing_part1` | Terminal 2 | The `time` output (`real` about 5 s) against `echo_server` |
| `p2_timing_part2` | Terminal 2 | The `time` output (`real` about 0.5 s) against `multi_threaded_server` |
| `p3_queue` | Terminal 1 | "queued ... (queue size: N)" rising to 10, then "dequeued" lines |
| `p3_shutdown` | Terminal 1 | "Shutdown requested: draining N queued client(s)" through "Shutdown complete" |
| `p3_top_pool` | Terminal 3 | `ps` / `top -H` for `thread_pool_server` (NLWP 11) |
| `p3_top_threads` | Terminal 3 | `ps` / `top -H` for `multi_threaded_server` (NLWP about 21) |
| `p4_browser_index` | Browser | Address bar, heading, grey background (CSS loaded), logo |
| `p4_browser_subpage` | Browser | Address bar showing `/subpage.html` |
| `p4_browser_404` | Browser | Address bar showing `/missing.txt` and "404 Not Found" |
| `p4_tests` | Terminal 2 | The whole `test_http.sh` table down to "All checks passed." |
| `p4_curl_index` | Terminal 2 | `curl -v /`: the `>` request lines, `< HTTP/1.1 200 OK`, the HTML |
| `p4_curl_css` | Terminal 2 | `< Content-Type: text/css` |
| `p4_curl_png` | Terminal 2 | `< Content-Type: image/png` |
| `p4_curl_404` | Terminal 2 | `< HTTP/1.1 404 Not Found` for `/missing.txt` |
| `p4_traversal` | Terminal 2 | Both traversal curl commands and their `404 Not Found` |
| `p4_traversal_log` | Terminal 1 | The `SECURITY: blocked path traversal attempt` lines |

Optional extra curl screenshots, included only if present: `p4_curl_indexhtml`,
`p4_curl_subpage`, `p4_curl_favicon`, `p4_curl_dotdot`, `p4_curl_query`, `p4_curl_longpath`.

## Troubleshooting

| Message | Fix |
|---------|-----|
| `bind: cannot use port 8080: Address already in use` | Another server is still running. Stop it with Ctrl+C or `pkill -f bin/`, or use another port. |
| `connect ... failed: Connection refused` | The server is not running, or the port does not match. |
| `Web root './www' is not a directory` | Start `http_server` from the project root. |
| `telnet: command not found` | `sudo apt install telnet`, or use `nc localhost 8080`. |
| Browser cannot reach the page from Windows (WSL) | Use `http://localhost:8080/`; if that fails, run `hostname -I` in WSL and use that address. |
