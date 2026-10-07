// Echo client used to test Parts 1-3.
//
// Sends one message, tells the server it is done (half-close), then prints
// everything the server echoes back.
//
// Usage: ./bin/echo_client <host> <port> <message>
#include "common.hpp"

#include <netdb.h>

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <host> <port> <message>" << std::endl;
        return 1;
    }
    std::string host = argv[1];
    std::string message = argv[3];
    int port = 0;
    if (!lab::parse_port(argv[2], port)) {
        std::cerr << "Invalid port '" << argv[2] << "' (must be 1-65535)" << std::endl;
        return 1;
    }

    // getaddrinfo resolves names like "localhost" as well as dotted IPs
    // (inet_pton alone only understands "127.0.0.1").
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result);
    if (rc != 0) {
        std::cerr << "Cannot resolve host '" << host << "': " << gai_strerror(rc) << std::endl;
        return 1;
    }

    int client_fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (client_fd < 0) {
        perror("socket");
        freeaddrinfo(result);
        return 1;
    }

    if (connect(client_fd, result->ai_addr, result->ai_addrlen) < 0) {
        std::cerr << "connect to " << host << ":" << port << " failed: " << std::strerror(errno)
                  << " (is the server running?)" << std::endl;
        freeaddrinfo(result);
        close(client_fd);
        return 1;
    }
    freeaddrinfo(result);

    if (!lab::write_all(client_fd, message.data(), message.size())) {
        perror("write");
        close(client_fd);
        return 1;
    }
    // Half-close: "I have nothing more to send". The server sees EOF, which
    // is how it knows the (possibly multi-line) message is complete.
    shutdown(client_fd, SHUT_WR);

    std::string echo;
    char buffer[1024];
    while (true) {
        ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer));
        if (bytes_read < 0) {
            if (errno == EINTR) continue;
            perror("read");
            close(client_fd);
            return 1;
        }
        if (bytes_read == 0) break;
        echo.append(buffer, static_cast<size_t>(bytes_read));
    }
    close(client_fd);

    // Long echoes are shortened on screen; the byte counts prove nothing was lost.
    const size_t max_shown = 200;
    if (echo.size() > max_shown) {
        std::cout << "Echo: " << echo.substr(0, max_shown) << "... [truncated on screen]" << std::endl;
    } else {
        std::cout << "Echo: " << echo << std::endl;
    }
    std::cout << "      (sent " << message.size() << " bytes, received " << echo.size() << " bytes"
              << (echo == message ? ", match" : ", MISMATCH") << ")" << std::endl;
    return echo == message ? 0 : 2;
}
