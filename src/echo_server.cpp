// Part 1: Simple echo server (single-threaded).
//
// Handles exactly one client at a time. While it is busy, other clients wait
// in the kernel's accept queue.
//
// Usage: ./bin/echo_server [port] [delay_ms]
#include "common.hpp"

int main(int argc, char* argv[]) {
    int port = 8080;   // Default port
    int delay_ms = 0;  // Simulated work per client
    if (!lab::parse_echo_server_args(argc, argv, port, delay_ms)) return 1;

    int server_fd = lab::create_listener(port);
    if (server_fd < 0) return 1;

    lab::log("Echo server (single-threaded) listening on port " + std::to_string(port) +
             (delay_ms > 0 ? ", delay " + std::to_string(delay_ms) + " ms per client" : ""));

    long total_connections = 0;
    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        std::string who = "Client #" + std::to_string(++total_connections);
        lab::log(who + " connected from " + lab::peer_name(client_addr));

        lab::simulate_work(delay_ms);
        size_t echoed = lab::echo_until_eof(client_fd, who);

        close(client_fd);
        lab::log(who + " disconnected (" + std::to_string(echoed) + " bytes echoed)");
    }

    close(server_fd);
    return 0;
}
