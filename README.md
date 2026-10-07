# Lab 4.1 – Client-Server Applications in C++

**Santiago Soto · M00121124 · CS 375 Operating Systems**

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

Without `make`, each program compiles from one file plus the shared header:

```bash
g++ -std=c++17 -pthread -o server src/echo_server.cpp
```

I developed and tested on Ubuntu 22.04.3 LTS (WSL2) with g++ 11.4.0 on a 16-core machine.

## Submission contents

| Item | Where |
|------|-------|
| Source code | [`src/`](src/) |
| Web files for Part 4 | [`www/`](www/) |
| Test scripts | [`scripts/`](scripts/) |
| Screenshots (36) | [`screenshots/`](screenshots/) |
| Report | [`report/report.pdf`](report/report.pdf), source in [`report/report.tex`](report/report.tex) |

`make zip` rebuilds the report and bundles all of the above into `Lab4.1_submission.zip`.

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
  test_http.sh               the 10 sample HTTP requests + 7 traversal/malformed-URL checks
screenshots/                 test evidence, named by part (p1_ … p4_)
report/
  report.tex                 LaTeX report; pulls screenshots in by file name
  report.pdf                 built report (22 pages)
Makefile                     make, make report, make zip, make clean
```

## Running each part

Every server takes the port as its first argument (default `8080`) and must be started from the
project root. **Ctrl+C** stops a server. Two terminals are needed: the server in one, the tests in
the other.

The echo servers accept an optional second argument, `delay_ms`, which simulates slow work per
client. Without it everything finishes in milliseconds and neither queuing nor concurrency is
visible.

### Part 1 – single-threaded echo server

```bash
./bin/echo_server 8080                              # terminal 1
./bin/echo_client localhost 8080 "Hello, server!"   # terminal 2
scripts/test_echo.sh 8080                           # all 10 sample requests
telnet localhost 8080                               # manual session; quit with Ctrl+] then "quit"
```

Queuing test (server restarted with a 2 s delay):

```bash
./bin/echo_server 8080 2000        # terminal 1
scripts/run_clients.sh 8080 3      # terminal 2 – takes ~6 s, one client at a time
```

### Part 2 – one thread per client

```bash
./bin/multi_threaded_server 8080 500    # terminal 1
time scripts/run_clients.sh 8080 10     # terminal 2
```

The Part 1 baseline uses the same delay: `./bin/echo_server 8080 500`.

### Part 3 – thread pool

```bash
./bin/thread_pool_server 8080 500     # terminal 1
scripts/run_clients.sh 8080 20        # terminal 2 – 20 clients, 10 workers
```

The log prints the queue size on every push and pop. Pressing Ctrl+C while clients are still
queued makes the server drain the queue before exiting. To show only the queue lines:

```bash
./bin/thread_pool_server 8080 500 | grep --line-buffered -E "listening|queue size"
```

Thread count and memory while clients are connected:

```bash
ps -o pid,nlwp,rss,pcpu,cmd -p "$(pgrep -f bin/thread_pool_server)"
```

### Part 4 – HTTP server

```bash
./bin/http_server 8080                  # terminal 1
scripts/test_http.sh 8080               # terminal 2 – all sample requests, pass/fail table
curl -v http://localhost:8080/          # a single request in detail
```

Browser: <http://localhost:8080/>

A different web root can be given as a second argument: `./bin/http_server 8080 /path/to/www`.

> **curl and `..`** – curl silently removes `../` from URLs before sending them. The
> `--path-as-is` flag sends the attack exactly as written:
> `curl -v --path-as-is http://localhost:8080/../../../etc/passwd`

## Measured results

Timing, with a 500 ms simulated delay per client:

| Server | Clients | Total time |
|--------|---------|-----------|
| Part 1 – single-threaded | 10 | 5.02 s |
| Part 2 – thread per client | 10 | 0.55 s |
| Part 3 – pool of 10 | 20 | 1.03 s |

Resource use with 20 clients connected (15 s delay), from `ps`:

| Server | Threads (NLWP) | Memory (RSS) | CPU |
|--------|----------------|--------------|-----|
| Part 2 – thread per client | 21 | 4348 kB | 0.0 % |
| Part 3 – pool of 10 | 11 | 4068 kB | 0.0 % |

In Part 3 the queue reached a maximum of 10 waiting clients. In Part 4 all 10 sample requests
returned the expected status and all 7 traversal and malformed-URL checks passed. The report
discusses these results.

## Lab requirements and where they are implemented

**All parts**

- Error handling: invalid/out-of-range ports (reported with the offending value), port already in
  use, unresolvable host, refused connection, partial writes, interrupted calls, clients that
  disconnect early (`common.hpp`).
- Logging: every event has a millisecond timestamp; the logger is mutex-protected so lines from
  different threads never mix (`lab::log`).

**Part 1**

- Multi-line and oversized messages: the server reads in a loop until EOF rather than doing a
  single 1024-byte read (`lab::echo_until_eof`). The client half-closes the socket
  (`shutdown(SHUT_WR)`) to signal the end of its message.
- The lab's client used `inet_pton`, which cannot resolve the name `localhost`. I replaced it with
  `getaddrinfo`, so `./bin/echo_client localhost 8080 ...` works as the lab sheet shows.

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
- Other statuses: `400` malformed request, `404` missing file, `408` slow client, `413` body over
  1 MiB, `431` headers over 8 KiB.
- A 5-second socket timeout stops an idle connection from occupying a worker.
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

The report reads its screenshots from `screenshots/` by file name (`.png`, `.jpg` or `.jpeg`),
so replacing a file and rebuilding updates the PDF. A file the report expects but cannot find
shows up as a red "Missing screenshot" box.

```bash
make report      # builds report/report.pdf and lists any screenshots still missing
```

Building needs LaTeX: `sudo apt install texlive-latex-recommended texlive-latex-extra`.

For Parts 1–3 the server terminal and the client terminal were captured separately. Where a test
has two files, the plain name is the server side and `_2` is the client side.

| File(s) in `screenshots/` | Shows |
|---|---|
| `p1_requests`, `p1_requests_2` | The 10 sample echo requests: server log and client output |
| `p1_telnet`, `p1_telnet_2` | Manual telnet session |
| `p1_queuing`, `p1_queuing_2` | 3 simultaneous clients served 2 s apart; 6.0 s in total |
| `p1_errors`, `p1_errors_2` | Invalid port, out-of-range port, connection refused |
| `p2_concurrent` | 10 parallel clients, interleaved log, connection and thread counters |
| `p2_timing_part1`, `p2_timing_part2` | `time` for 10 clients against Part 1 and Part 2 |
| `p3_queue_sizes` | Queue lines only: size rising to 10, then falling to 0 |
| `p3_queue`, `p3_queue_2`, `p3_queue_time` | Full server log, client output and total time for 20 clients |
| `p3_shutdown` | Ctrl+C with a client still queued; queue drained before exit |
| `p3_top_threads`, `p3_top_pool` | `ps` output under load for Part 2 and Part 3 |
| `p4_browser_index`, `p4_browser_subpage`, `p4_browser_404` | Pages in the browser |
| `p4_browser_log` | Server log of the browser's requests, handled by several workers |
| `p4_tests` | `test_http.sh` table: all 17 checks passed |
| `p4_curl_index`, `p4_curl_indexhtml`, `p4_curl_css`, `p4_curl_png`, `p4_curl_subpage`, `p4_curl_404`, `p4_curl_favicon`, `p4_curl_dotdot`, `p4_curl_query`, `p4_curl_longpath` | `curl -v` for each of the 10 sample requests |
| `p4_traversal`, `p4_traversal_2` | Traversal attempts, plain and percent-encoded: both `404` |
| `p4_traversal_log` | The `SECURITY: blocked path traversal attempt` log lines |

## Troubleshooting

| Message | Fix |
|---------|-----|
| `bind: cannot use port 8080: Address already in use` | Another server is still running. Stop it with Ctrl+C, or use another port. |
| `connect ... failed: Connection refused` | The server is not running, or the port does not match. |
| `Web root './www' is not a directory` | Start `http_server` from the project root, or pass the directory as the second argument. |
| `telnet: command not found` | `sudo apt install telnet`, or use `nc localhost 8080`. |
| Browser cannot reach the page from Windows (WSL) | Use `http://localhost:8080/`; if that fails, run `hostname -I` in WSL and use that address. |
