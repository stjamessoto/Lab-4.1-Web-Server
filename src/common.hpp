// common.hpp - helpers shared by every program in Lab 4.1.
//
// Keeps the socket setup, logging and error handling in one place so each
// part's .cpp file only contains what is new in that part.
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <mutex>
#include <string>

namespace lab {

// ---------------------------------------------------------------- logging

// "2026-10-05 14:03:22.417" - millisecond timestamps make the ordering of
// concurrent clients visible in screenshots.
inline std::string timestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char date[32];
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &tm);
    char out[48];
    std::snprintf(out, sizeof(out), "%s.%03d", date, static_cast<int>(ms.count()));
    return out;
}

// Thread-safe: the mutex stops lines from different threads interleaving
// mid-line.
inline void log(const std::string& msg) {
    static std::mutex log_mutex;
    std::lock_guard<std::mutex> lock(log_mutex);
    std::cout << "[" << timestamp() << "] " << msg << std::endl;
}

// Makes a message safe and short enough for one log line: control characters
// are escaped and anything past max_len is cut off.
inline std::string preview(const std::string& data, size_t max_len = 80) {
    std::string out;
    for (size_t i = 0; i < data.size() && out.size() < max_len; ++i) {
        unsigned char c = static_cast<unsigned char>(data[i]);
        if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20 || c == 0x7f) out += '.';
        else out += static_cast<char>(c);
    }
    if (data.size() > max_len) out += "...";
    return out;
}

// ------------------------------------------------------- argument parsing

// Accepts only a whole number in [min, max]; rejects "80abc", "", "-1", etc.
inline bool parse_int(const char* text, long min, long max, int& out) {
    if (text == nullptr || *text == '\0') return false;
    char* end = nullptr;
    errno = 0;
    long value = std::strtol(text, &end, 10);
    if (errno != 0 || *end != '\0' || value < min || value > max) return false;
    out = static_cast<int>(value);
    return true;
}

inline bool parse_port(const char* text, int& port) {
    return parse_int(text, 1, 65535, port);
}

// Echo servers (Parts 1-3) share the same command line: [port] [delay_ms].
// delay_ms simulates slow work per client so queuing/concurrency is easy to
// see and to time.
inline bool parse_echo_server_args(int argc, char* argv[], int& port, int& delay_ms) {
    std::string problem;
    if (argc > 3) {
        problem = "Too many arguments";
    } else if (argc > 1 && !parse_port(argv[1], port)) {
        problem = std::string("Invalid port '") + argv[1] + "' (must be a number from 1 to 65535)";
    } else if (argc > 2 && !parse_int(argv[2], 0, 60000, delay_ms)) {
        problem = std::string("Invalid delay '") + argv[2] + "' (must be a number from 0 to 60000)";
    }
    if (!problem.empty()) {
        std::cerr << "Error: " << problem << "\n"
                  << "Usage: " << argv[0] << " [port] [delay_ms]\n"
                  << "  port      1-65535 (default 8080)\n"
                  << "  delay_ms  0-60000, simulated work per client (default 0)\n";
        return false;
    }
    return true;
}

inline void simulate_work(int delay_ms) {
    if (delay_ms > 0) usleep(static_cast<useconds_t>(delay_ms) * 1000);
}

// ----------------------------------------------------------- socket setup

// socket() + bind() + listen(). Returns the listening fd, or -1 after
// printing the reason.
inline int create_listener(int port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return -1;
    }

    // Lets the server restart immediately instead of failing with
    // "Address already in use" while the old socket is in TIME_WAIT.
    int yes = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "bind: cannot use port " << port << ": " << std::strerror(errno) << "\n";
        if (errno == EADDRINUSE) std::cerr << "  (another server is already running on this port)\n";
        if (errno == EACCES) std::cerr << "  (ports below 1024 need root; try 8080)\n";
        close(server_fd);
        return -1;
    }

    // The backlog is how many finished connections may wait for accept().
    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("listen");
        close(server_fd);
        return -1;
    }
    return server_fd;
}

inline std::string peer_name(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

// ------------------------------------------------------------- socket I/O

// write() may send only part of the buffer; loop until everything is out.
// MSG_NOSIGNAL: a client that disconnects early gives an error, not SIGPIPE.
inline bool write_all(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Echoes everything the client sends until it closes its side (EOF). Reading
// in a loop is what makes multi-line messages and messages longer than the
// 1024-byte buffer work. Returns the number of bytes echoed.
inline size_t echo_until_eof(int client_fd, const std::string& who) {
    char buffer[1024];
    size_t total = 0;
    while (true) {
        ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer));
        if (bytes_read < 0) {
            if (errno == EINTR) continue;
            log(who + " read error: " + std::strerror(errno));
            break;
        }
        if (bytes_read == 0) break;  // EOF
        log(who + " received " + std::to_string(bytes_read) + " bytes: " +
            preview(std::string(buffer, static_cast<size_t>(bytes_read))));
        if (!write_all(client_fd, buffer, static_cast<size_t>(bytes_read))) {
            log(who + " write error: " + std::strerror(errno));
            break;
        }
        total += static_cast<size_t>(bytes_read);
    }
    return total;
}

// ------------------------------------------------- graceful shutdown (3, 4)

inline volatile std::sig_atomic_t g_shutdown_requested = 0;

inline void on_shutdown_signal(int) { g_shutdown_requested = 1; }

inline bool shutdown_requested() { return g_shutdown_requested != 0; }

// Ctrl+C / kill set a flag instead of killing the process. No SA_RESTART, so
// a blocked accept() returns EINTR and the main loop can notice the flag.
inline void install_shutdown_handler() {
    struct sigaction sa{};
    sa.sa_handler = on_shutdown_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

// Threads inherit the signal mask. Blocking the signals while workers are
// created guarantees they are delivered to the main thread (the one sitting
// in accept()).
inline void set_shutdown_signals_blocked(bool blocked) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(blocked ? SIG_BLOCK : SIG_UNBLOCK, &set, nullptr);
}

}  // namespace lab
