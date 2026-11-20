#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <thread>

namespace aiws {

/**
 * @brief Receives Remote Add requests on its own std::thread.
 *
 * The handler runs on the server thread and must not wait for the GUI thread.
 * No Qt types appear here, so the class can be tested without a window.
 */
class IpcServer {
public:
    using AddHandler = std::function<void(const std::string& path)>;

    IpcServer(std::string socket_path, AddHandler on_add);
    ~IpcServer();

    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    /** @brief Binds the socket and starts the thread. False, with error set, on failure. */
    bool start(std::string& error);

    /** @brief Stops serving, joins the thread, and removes the socket file. Idempotent. */
    void stop();

private:
    void serve();
    void handle_client(int client_fd);
    bool read_exact(int fd, unsigned char* buffer, std::size_t count);

    std::string socket_path_;
    AddHandler on_add_;
    // All three are set by start() before the thread exists and closed by
    // stop() after it is joined, so neither thread writes them while both run.
    int listen_fd_{-1};
    int wake_read_fd_{-1};   // The server thread waits on this with poll().
    int wake_write_fd_{-1};  // stop() writes one byte here to wake it.
    std::thread thread_;
};

}  // namespace aiws
