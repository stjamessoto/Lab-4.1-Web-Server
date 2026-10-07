// Part 3: Thread-pool echo server.
//
// A fixed pool of NUM_THREADS workers shares one task queue. The main thread
// only accepts connections and pushes them onto the queue; workers pop and
// serve them. Resource use stays bounded no matter how many clients arrive.
//
// Ctrl+C triggers a graceful shutdown: stop accepting, let the workers drain
// the queue, then exit.
//
// Usage: ./bin/thread_pool_server [port] [delay_ms]
#include "common.hpp"

#include <condition_variable>
#include <queue>
#include <thread>
#include <vector>

struct Task {
    int client_fd;
    long id;
};

std::queue<Task> task_queue;  // Clients waiting for a worker
std::mutex queue_mutex;
std::condition_variable cv;
bool stop_pool = false;
const int NUM_THREADS = 10;

static int g_delay_ms = 0;

void worker_thread(int worker_id) {
    std::string me = "Worker " + std::to_string(worker_id);
    while (true) {
        Task task{};
        size_t queue_size = 0;
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            cv.wait(lock, [] { return !task_queue.empty() || stop_pool; });
            if (stop_pool && task_queue.empty()) return;  // Queue drained
            task = task_queue.front();
            task_queue.pop();
            queue_size = task_queue.size();
        }

        std::string who = me + " / Client #" + std::to_string(task.id);
        lab::log(who + " dequeued (queue size: " + std::to_string(queue_size) + ")");

        // Handle client
        lab::simulate_work(g_delay_ms);
        size_t echoed = lab::echo_until_eof(task.client_fd, who);
        close(task.client_fd);
        lab::log(who + " done (" + std::to_string(echoed) + " bytes echoed)");
    }
}

int main(int argc, char* argv[]) {
    int port = 8080;
    if (!lab::parse_echo_server_args(argc, argv, port, g_delay_ms)) return 1;

    int server_fd = lab::create_listener(port);
    if (server_fd < 0) return 1;

    lab::install_shutdown_handler();

    // Workers are created with SIGINT/SIGTERM blocked so the signal always
    // lands on this thread and interrupts accept().
    lab::set_shutdown_signals_blocked(true);
    std::vector<std::thread> workers;
    for (int i = 0; i < NUM_THREADS; ++i) {
        workers.emplace_back(worker_thread, i + 1);
    }
    lab::set_shutdown_signals_blocked(false);

    lab::log("Thread-pool echo server listening on port " + std::to_string(port) + " with " +
             std::to_string(NUM_THREADS) + " workers" +
             (g_delay_ms > 0 ? ", delay " + std::to_string(g_delay_ms) + " ms per client" : "") +
             " (Ctrl+C to shut down)");

    long total_connections = 0;
    while (!lab::shutdown_requested()) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;  // Signal: the loop condition decides
            perror("accept");
            continue;
        }

        long id = ++total_connections;
        size_t queue_size = 0;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            task_queue.push(Task{client_fd, id});
            queue_size = task_queue.size();
        }
        lab::log("Client #" + std::to_string(id) + " queued from " + lab::peer_name(client_addr) +
                 " (queue size: " + std::to_string(queue_size) + ")");
        cv.notify_one();
    }

    // Cleanup: stop accepting, let workers finish what is already queued.
    close(server_fd);
    size_t pending = 0;
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        stop_pool = true;
        pending = task_queue.size();
    }
    lab::log("Shutdown requested: draining " + std::to_string(pending) + " queued client(s)...");
    cv.notify_all();
    for (auto& w : workers) w.join();

    lab::log("Shutdown complete. Total connections served: " + std::to_string(total_connections));
    return 0;
}
