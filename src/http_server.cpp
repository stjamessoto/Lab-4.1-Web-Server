// Part 4: Simple HTTP web server.
//
// Built on the Part 3 thread pool. Serves static files from a web root
// (./www by default) for GET/HEAD, echoes POST bodies, and defends against
// path traversal (see resolve_path).
//
// Usage: ./bin/http_server [port] [web_root]
#include "common.hpp"

#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <queue>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

// ------------------------------------------------------------ thread pool

struct Task {
    int client_fd;
    long id;
    std::string peer;
};

std::queue<Task> task_queue;
std::mutex queue_mutex;
std::condition_variable cv;
bool stop_pool = false;
const int NUM_THREADS = 10;

// ----------------------------------------------------------------- limits

const size_t MAX_HEADER_BYTES = 8192;       // Request line + headers
const size_t MAX_BODY_BYTES = 1024 * 1024;  // POST body
const int IO_TIMEOUT_SECONDS = 5;           // Idle clients must not pin a worker

// Canonical (absolute, symlink-free) web root, set once in main().
static fs::path g_web_root;

// ---------------------------------------------------------------- request

struct Request {
    std::string method;
    std::string target;   // Exactly as sent, e.g. "/a%20b.html?x=1"
    std::string path;     // Decoded, without the query, e.g. "/a b.html"
    std::string query;    // Raw text after '?', e.g. "x=1"
    std::string version;
    std::map<std::string, std::string> headers;  // Names lower-cased
    std::vector<std::pair<std::string, std::string>> params;  // Decoded query
    std::string body;
    std::string raw_head;  // Request line + headers, for logging
};

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Percent-decoding: "%20" -> ' ', "%2e%2e" -> "..". Fails on malformed
// escapes ("%zz", a trailing "%") and on "%00", which has no legitimate use
// in a file path. Decoding happens exactly once, before any security check.
bool url_decode(const std::string& in, std::string& out, bool plus_is_space) {
    out.clear();
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%') {
            if (i + 2 >= in.size()) return false;
            int hi = hex_value(in[i + 1]);
            int lo = hex_value(in[i + 2]);
            if (hi < 0 || lo < 0) return false;
            char decoded = static_cast<char>(hi * 16 + lo);
            if (decoded == '\0') return false;
            out += decoded;
            i += 2;
        } else if (in[i] == '+' && plus_is_space) {
            out += ' ';
        } else {
            out += in[i];
        }
    }
    return true;
}

// "a=1&b=hello+world" -> {("a","1"), ("b","hello world")}
std::vector<std::pair<std::string, std::string>> parse_query(const std::string& query) {
    std::vector<std::pair<std::string, std::string>> params;
    std::istringstream stream(query);
    std::string pair;
    while (std::getline(stream, pair, '&')) {
        if (pair.empty()) continue;
        size_t eq = pair.find('=');
        std::string key, value;
        // Undecodable pieces are kept raw rather than dropped.
        if (!url_decode(pair.substr(0, eq), key, true)) key = pair.substr(0, eq);
        if (eq != std::string::npos && !url_decode(pair.substr(eq + 1), value, true)) {
            value = pair.substr(eq + 1);
        }
        params.emplace_back(key, value);
    }
    return params;
}

// Reads and parses one HTTP request.
// Returns 200 when `req` is filled in, an HTTP error status (400, 408, 413,
// 431) when the request is unusable, or 0 when the client connected and left
// without sending anything (nothing to answer).
int parse_request(int client_fd, Request& req) {
    // 1. Read until the blank line that ends the headers. One read() is not
    //    enough: TCP may deliver the request in several pieces.
    std::string data;
    size_t head_end = std::string::npos;
    size_t body_start = 0;
    char buffer[4096];
    while (true) {
        size_t crlf = data.find("\r\n\r\n");
        size_t lf = data.find("\n\n");  // Tolerate bare-LF clients (nc, printf)
        if (crlf != std::string::npos && (lf == std::string::npos || crlf < lf)) {
            head_end = crlf;
            body_start = crlf + 4;
            break;
        }
        if (lf != std::string::npos) {
            head_end = lf;
            body_start = lf + 2;
            break;
        }
        if (data.size() > MAX_HEADER_BYTES) return 431;

        ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return data.empty() ? 0 : 408;
            return 0;
        }
        if (n == 0) return data.empty() ? 0 : 400;  // Closed mid-request
        data.append(buffer, static_cast<size_t>(n));
    }
    if (head_end > MAX_HEADER_BYTES) return 431;
    req.raw_head = data.substr(0, head_end);

    // 2. Split the head into lines.
    std::vector<std::string> lines;
    std::istringstream head(req.raw_head);
    std::string line;
    while (std::getline(head, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    if (lines.empty()) return 400;

    // 3. Request line: METHOD SP TARGET SP VERSION, nothing else.
    std::istringstream first_line(lines[0]);
    std::string extra;
    if (!(first_line >> req.method >> req.target >> req.version) || (first_line >> extra)) return 400;
    if (req.version != "HTTP/1.1" && req.version != "HTTP/1.0") return 400;
    if (req.target.empty() || req.target[0] != '/') return 400;

    // 4. Headers: "Name: value".
    for (size_t i = 1; i < lines.size(); ++i) {
        size_t colon = lines[i].find(':');
        if (colon == std::string::npos || colon == 0) return 400;
        std::string name = lines[i].substr(0, colon);
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        size_t value_start = lines[i].find_first_not_of(" \t", colon + 1);
        std::string value = value_start == std::string::npos ? "" : lines[i].substr(value_start);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
        req.headers[name] = value;
    }

    // 5. Target: drop any fragment, split off the query, decode the path.
    std::string target = req.target.substr(0, req.target.find('#'));
    size_t question = target.find('?');
    if (question != std::string::npos) {
        req.query = target.substr(question + 1);
        req.params = parse_query(req.query);
    }
    if (!url_decode(target.substr(0, question), req.path, false)) return 400;

    // 6. Body (POST): exactly Content-Length bytes.
    auto length_header = req.headers.find("content-length");
    if (length_header != req.headers.end()) {
        const std::string& text = length_header->second;
        if (text.empty() || text.size() > 9 ||
            !std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); })) {
            return 400;
        }
        size_t length = std::stoul(text);
        if (length > MAX_BODY_BYTES) return 413;

        req.body = data.substr(body_start);
        while (req.body.size() < length) {
            ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 408;
            if (n <= 0) return 400;
            req.body.append(buffer, static_cast<size_t>(n));
        }
        req.body.resize(length);
    }
    return 200;
}

// ------------------------------------------------------------ file access

// Maps a decoded URL path onto a file inside the web root.
//
// SECURITY (path traversal): searching the URL for ".." is not enough -
// "%2e%2e" hides it until decoding, and a symlink can leave the root with
// no ".." at all. So instead of looking for bad input, we compute where the
// request really ends up and verify the result:
//   1. the path has already been percent-decoded (parse_request),
//   2. weakly_canonical() makes it absolute and resolves "." / ".." /
//      symlinks,
//   3. the result must still be located inside the canonical web root.
// Anything else returns nullopt and the client gets a 404.
std::optional<fs::path> resolve_path(const std::string& url_path) {
    std::error_code ec;
    // relative_path() strips the leading '/', otherwise operator/ would
    // discard the root and treat the URL as an absolute filesystem path.
    fs::path resolved = fs::weakly_canonical(g_web_root / fs::path(url_path).relative_path(), ec);
    if (ec) return std::nullopt;

    // Component-wise prefix test ("/srv/www-secret" must not pass as being
    // inside "/srv/www", which a plain string prefix test would allow).
    auto mismatch = std::mismatch(g_web_root.begin(), g_web_root.end(), resolved.begin(), resolved.end());
    if (mismatch.first != g_web_root.end()) {
        lab::log("SECURITY: blocked path traversal attempt: '" + lab::preview(url_path) + "' -> '" +
                 lab::preview(resolved.string()) + "'");
        return std::nullopt;
    }

    // "/" and other directories serve their index.html.
    if (fs::is_directory(resolved, ec)) resolved /= "index.html";
    if (!fs::is_regular_file(resolved, ec)) return std::nullopt;
    return resolved;
}

// Returns the file's bytes (binary-safe, so images work), or nullopt when
// the URL does not name a readable file inside the web root. `resolved`
// receives the real file path so the caller can pick a Content-Type.
std::optional<std::string> get_file_content(const std::string& url_path, fs::path& resolved) {
    std::optional<fs::path> fs_path = resolve_path(url_path);
    if (!fs_path) return std::nullopt;

    std::ifstream file(*fs_path, std::ios::binary);
    if (!file.is_open()) return std::nullopt;
    std::stringstream buffer;
    buffer << file.rdbuf();
    resolved = *fs_path;
    return buffer.str();
}

std::string get_content_type(const fs::path& file) {
    static const std::map<std::string, std::string> types = {
        {".html", "text/html; charset=utf-8"},
        {".htm", "text/html; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"},
        {".json", "application/json"},
        {".txt", "text/plain; charset=utf-8"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif", "image/gif"},
        {".svg", "image/svg+xml"},
        {".ico", "image/x-icon"},
        {".pdf", "application/pdf"},
    };
    std::string ext = file.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto it = types.find(ext);
    return it != types.end() ? it->second : "application/octet-stream";
}

// --------------------------------------------------------------- response

std::string reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 431: return "Request Header Fields Too Large";
        default:  return "Internal Server Error";
    }
}

// Error pages never include anything the client sent, so they cannot be
// used to inject HTML/script into the response.
std::string error_page(int status) {
    std::string title = std::to_string(status) + " " + reason_phrase(status);
    return "<!DOCTYPE html>\n<html>\n<head><title>" + title + "</title></head>\n<body>\n<h1>" + title +
           "</h1>\n<p>Lab 4.1 C++ web server</p>\n</body>\n</html>\n";
}

void send_response(int client_fd, int status, const std::string& content,
                   const std::string& content_type = "text/html; charset=utf-8",
                   bool head_only = false, const std::string& extra_headers = "") {
    std::string response = "HTTP/1.1 " + std::to_string(status) + " " + reason_phrase(status) + "\r\n";
    response += "Server: Lab4.1-http_server\r\n";
    response += "Content-Type: " + content_type + "\r\n";
    response += "Content-Length: " + std::to_string(content.length()) + "\r\n";
    response += "X-Content-Type-Options: nosniff\r\n";
    response += extra_headers;
    response += "Connection: close\r\n\r\n";
    if (!head_only) response += content;  // HEAD: headers only

    lab::write_all(client_fd, response.data(), response.size());
}

// ----------------------------------------------------------------- worker

void handle_client(const Task& task, const std::string& me) {
    std::string who = "#" + std::to_string(task.id) + " " + task.peer;

    Request req;
    int status = parse_request(task.client_fd, req);
    if (status == 0) {
        lab::log(who + " closed without sending a request [" + me + "]");
        return;
    }

    std::string content;
    std::string content_type = "text/html; charset=utf-8";
    std::string extra_headers;

    if (status != 200) {
        content = error_page(status);
    } else if (req.method == "GET" || req.method == "HEAD") {
        fs::path file;
        std::optional<std::string> file_content = get_file_content(req.path, file);
        if (file_content) {
            content = std::move(*file_content);
            content_type = get_content_type(file);
        } else {
            status = 404;
            content = error_page(404);
        }
    } else if (req.method == "POST") {
        content = "Received POST to " + req.path + " with " + std::to_string(req.body.size()) +
                  " byte(s):\n" + req.body + "\n";
        content_type = "text/plain; charset=utf-8";
    } else {
        status = 405;
        content = error_page(405);
        extra_headers = "Allow: GET, HEAD, POST\r\n";
    }

    send_response(task.client_fd, status, content, content_type, req.method == "HEAD", extra_headers);

    // One block per request so lines from different workers do not mix.
    std::ostringstream entry;
    entry << who << " \"" << lab::preview(req.raw_head.substr(0, req.raw_head.find_first_of("\r\n")), 120) << "\" -> "
          << status << " " << reason_phrase(status) << " (" << content.size() << " bytes, " << content_type
          << ") [" << me << "]";
    std::istringstream head(req.raw_head);
    std::string line;
    while (std::getline(head, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        entry << "\n      | " << lab::preview(line, 200);
    }
    for (const auto& param : req.params) {
        entry << "\n      ? " << lab::preview(param.first) << " = " << lab::preview(param.second);
    }
    if (!req.body.empty()) entry << "\n      body: " << lab::preview(req.body, 200);
    lab::log(entry.str());
}

void worker_thread(int worker_id) {
    std::string me = "worker " + std::to_string(worker_id);
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            cv.wait(lock, [] { return !task_queue.empty() || stop_pool; });
            if (stop_pool && task_queue.empty()) return;
            task = std::move(task_queue.front());
            task_queue.pop();
        }
        handle_client(task, me);
        close(task.client_fd);
    }
}

// ------------------------------------------------------------------- main

int main(int argc, char* argv[]) {
    int port = 8080;
    std::string web_root = "./www";
    if (argc > 3 || (argc > 1 && !lab::parse_port(argv[1], port))) {
        if (argc <= 3) std::cerr << "Error: Invalid port '" << argv[1] << "' (must be a number from 1 to 65535)\n";
        std::cerr << "Usage: " << argv[0] << " [port] [web_root]\n"
                  << "  port      1-65535 (default 8080)\n"
                  << "  web_root  directory to serve (default ./www)\n";
        return 1;
    }
    if (argc > 2) web_root = argv[2];

    std::error_code ec;
    g_web_root = fs::canonical(web_root, ec);
    if (ec || !fs::is_directory(g_web_root)) {
        std::cerr << "Web root '" << web_root << "' is not a directory.\n"
                  << "  Run the server from the project root (the folder that contains www/),\n"
                  << "  or pass the directory: " << argv[0] << " " << port << " /path/to/www\n";
        return 1;
    }

    int server_fd = lab::create_listener(port);
    if (server_fd < 0) return 1;

    lab::install_shutdown_handler();
    lab::set_shutdown_signals_blocked(true);
    std::vector<std::thread> workers;
    for (int i = 0; i < NUM_THREADS; ++i) {
        workers.emplace_back(worker_thread, i + 1);
    }
    lab::set_shutdown_signals_blocked(false);

    lab::log("HTTP server listening on http://localhost:" + std::to_string(port) + "/ serving " +
             g_web_root.string() + " with " + std::to_string(NUM_THREADS) + " workers (Ctrl+C to shut down)");

    long total_connections = 0;
    while (!lab::shutdown_requested()) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        // A client that connects and then stays silent (or never reads the
        // response) would otherwise hold a worker forever.
        timeval timeout{};
        timeout.tv_sec = IO_TIMEOUT_SECONDS;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            task_queue.push(Task{client_fd, ++total_connections, lab::peer_name(client_addr)});
        }
        cv.notify_one();
    }

    // Cleanup: stop accepting, let workers finish what is already queued.
    close(server_fd);
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        stop_pool = true;
    }
    lab::log("Shutdown requested: finishing queued requests...");
    cv.notify_all();
    for (auto& w : workers) w.join();

    lab::log("Shutdown complete. Total connections served: " + std::to_string(total_connections));
    return 0;
}
