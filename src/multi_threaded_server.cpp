// Part 2: Multi-threaded echo server (one pthread per client).
//
// Every accepted client gets its own thread, so slow clients no longer block
// the others. The cost: one thread (stack, scheduling) per connection.
//
// Usage: ./bin/multi_threaded_server [port] [delay_ms]
#include "common.hpp"

// Shared state, touched by every client thread -> protected by a mutex.
static pthread_mutex_t g_stats_mutex = PTHREAD_MUTEX_INITIALIZER;
static long g_total_connections = 0;
static int g_active_threads = 0;

// Refuse new clients past this point instead of creating threads until the
// process runs out of memory.
static const int MAX_THREADS = 200;

struct ClientArgs {
    int client_fd;
    long id;
    int delay_ms;
};

void* handle_client(void* arg) {
    ClientArgs* args = static_cast<ClientArgs*>(arg);
    int client_fd = args->client_fd;
    std::string who = "Client #" + std::to_string(args->id);
    int delay_ms = args->delay_ms;
    delete args;  // Free the heap-allocated arguments

    lab::simulate_work(delay_ms);
    size_t echoed = lab::echo_until_eof(client_fd, who);
    close(client_fd);

    pthread_mutex_lock(&g_stats_mutex);
    int active = --g_active_threads;
    pthread_mutex_unlock(&g_stats_mutex);

    lab::log(who + " disconnected (" + std::to_string(echoed) + " bytes echoed, " +
             std::to_string(active) + " threads still active)");
    return nullptr;
}

int main(int argc, char* argv[]) {
    int port = 8080;
    int delay_ms = 0;
    if (!lab::parse_echo_server_args(argc, argv, port, delay_ms)) return 1;

    int server_fd = lab::create_listener(port);
    if (server_fd < 0) return 1;

    lab::log("Multi-threaded echo server listening on port " + std::to_string(port) +
             (delay_ms > 0 ? ", delay " + std::to_string(delay_ms) + " ms per client" : ""));

    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        pthread_mutex_lock(&g_stats_mutex);
        bool over_limit = g_active_threads >= MAX_THREADS;
        long id = 0;
        int active = g_active_threads;
        if (!over_limit) {
            id = ++g_total_connections;
            active = ++g_active_threads;
        }
        pthread_mutex_unlock(&g_stats_mutex);

        if (over_limit) {
            lab::log("Rejected " + lab::peer_name(client_addr) + ": thread limit (" +
                     std::to_string(MAX_THREADS) + ") reached");
            close(client_fd);
            continue;
        }

        lab::log("Client #" + std::to_string(id) + " connected from " + lab::peer_name(client_addr) +
                 " (total connections: " + std::to_string(id) +
                 ", active threads: " + std::to_string(active) + ")");

        ClientArgs* args = new ClientArgs{client_fd, id, delay_ms};
        pthread_t thread;
        int rc = pthread_create(&thread, nullptr, handle_client, args);
        if (rc != 0) {
            // pthread_create returns the error instead of setting errno.
            lab::log(std::string("pthread_create failed: ") + std::strerror(rc));
            close(client_fd);
            delete args;
            pthread_mutex_lock(&g_stats_mutex);
            --g_active_threads;
            pthread_mutex_unlock(&g_stats_mutex);
            continue;
        }
        pthread_detach(thread);  // No need to join
    }

    close(server_fd);
    return 0;
}
