#include "aiws/ipc_server.hpp"

#include "aiws/ipc_protocol.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <initializer_list>
#include <utility>

namespace aiws {

namespace {

constexpr std::chrono::milliseconds kClientIdleTimeout{3000};  // Bounds a silent client.

bool fill_address(const std::string& path, sockaddr_un& address) {
    if (path.size() >= sizeof(address.sun_path)) {
        return false;
    }
    std::memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    return true;
}

// True if a live server already answers on this socket.
bool socket_is_live(const sockaddr_un& address) {
    int probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (probe < 0) {
        return false;
    }
    bool live = ::connect(probe, reinterpret_cast<const sockaddr*>(&address),
                          sizeof(address)) == 0;
    ::close(probe);
    return live;
}

// Empty means readable. Never blocks: O_NONBLOCK guards against a FIFO.
std::string why_unreadable(const std::string& path) {
    if (path.empty() || path.front() != '/' ||
        path.find('\0') != std::string::npos) {
        return "invalid path";
    }
    int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return std::strerror(errno);
    }
    struct stat info {};
    std::string reason;
    if (::fstat(fd, &info) != 0) {
        reason = std::strerror(errno);
    } else if (!S_ISREG(info.st_mode)) {
        reason = "not a regular file";
    }
    ::close(fd);
    return reason;
}

void send_reply(int fd, unsigned char status, std::string reason) {
    if (reason.size() > ipc::kMaxReasonBytes) {
        reason.resize(ipc::kMaxReasonBytes);
    }

    std::string message(1, static_cast<char>(status));
    ipc::append_u32(message, static_cast<std::uint32_t>(reason.size()));
    message += reason;

    // MSG_NOSIGNAL prevents SIGPIPE if the client has disconnected.
    std::size_t offset = 0;
    while (offset < message.size()) {
        const auto count = ::send(fd, message.data() + offset,
                                  message.size() - offset,
                                  MSG_NOSIGNAL | MSG_DONTWAIT);
        if (count <= 0) {
            break;
        }
        offset += static_cast<std::size_t>(count);
    }
}

}  // namespace

IpcServer::IpcServer(std::string socket_path, AddHandler on_add)
    : socket_path_(std::move(socket_path)), on_add_(std::move(on_add)) {}

IpcServer::~IpcServer() { stop(); }

bool IpcServer::start(std::string& error) {
    sockaddr_un address{};
    if (!fill_address(socket_path_, address)) {
        error = "socket path is too long";
        return false;
    }

    if (socket_is_live(address)) {
        error = "another application is serving " + socket_path_;
        return false;
    }

    // A leftover socket from a crashed run is safe to remove. Anything else
    // at this path is not ours, so bind() will fail and report it.
    struct stat info {};
    if (::lstat(socket_path_.c_str(), &info) == 0 && S_ISSOCK(info.st_mode)) {
        ::unlink(socket_path_.c_str());
    }

    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        error = std::strerror(errno);
        return false;
    }
    if (::bind(listen_fd_, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) != 0 ||
        ::listen(listen_fd_, 16) != 0) {
        error = std::strerror(errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    ::chmod(socket_path_.c_str(), 0600);

    int wake[2];
    if (::pipe2(wake, O_CLOEXEC) != 0) {
        error = std::strerror(errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        ::unlink(socket_path_.c_str());
        return false;
    }
    wake_read_fd_ = wake[0];
    wake_write_fd_ = wake[1];

    thread_ = std::thread(&IpcServer::serve, this);
    return true;
}

void IpcServer::stop() {
    if (thread_.joinable()) {
        const char byte = 1;
        ssize_t written = ::write(wake_write_fd_, &byte, 1);  // Wakes poll() at once.
        (void)written;
        thread_.join();
    }
    for (int* fd : {&wake_read_fd_, &wake_write_fd_}) {
        if (*fd >= 0) {
            ::close(*fd);
            *fd = -1;
        }
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        ::unlink(socket_path_.c_str());
    }
}

// Blocks in poll() until a client connects or stop() writes to the wake pipe.
void IpcServer::serve() {
    while (true) {
        pollfd fds[] = {
            {listen_fd_, POLLIN, 0},
            {wake_read_fd_, POLLIN, 0}
        };

        if (::poll(fds, 2, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (fds[1].revents != 0) {
            break;  // stop() woke the thread.
        }
        if (!(fds[0].revents & POLLIN)) {
            continue;
        }

        const int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (client >= 0) {
            handle_client(client);
            ::close(client);
        }
    }
}

// Blocks in poll() on the client and the wake pipe, so stop() is honored even
// mid-request. Reassembles data that arrives in pieces (E3).
bool IpcServer::read_exact(int fd, unsigned char* buffer, std::size_t count) {
    using Clock = std::chrono::steady_clock;
    auto deadline = Clock::now() + kClientIdleTimeout;
    std::size_t received = 0;

    while (received < count) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - Clock::now());
        if (left.count() <= 0) {
            return false;  // Silent client.
        }
        pollfd waiting[2] = {{fd, POLLIN, 0}, {wake_read_fd_, POLLIN, 0}};
        int ready = ::poll(waiting, 2, static_cast<int>(left.count()));
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready < 0 || waiting[1].revents != 0) {
            return false;  // Error, or stop() was called.
        }
        if (ready == 0) {
            continue;  // Deadline reached; the check above ends the wait.
        }
        ssize_t n = ::recv(fd, buffer + received, count - received, 0);
        if (n == 0) {
            return false;  // Client closed early.
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        received += static_cast<std::size_t>(n);
        deadline = Clock::now() + kClientIdleTimeout;
    }
    return true;
}

void IpcServer::handle_client(int client_fd) {
    unsigned char header[4];
    if (!read_exact(client_fd, header, sizeof(header))) {
        return;
    }

    const auto length = ipc::read_u32(header);
    if (length == 0 || length > ipc::kMaxPathBytes) {
        send_reply(client_fd, ipc::kStatusRejected, "invalid path length");
        return;
    }

    std::string path(length, '\0');
    if (!read_exact(client_fd,
                    reinterpret_cast<unsigned char*>(&path[0]), path.size())) {
        return;
    }

    const auto error = why_unreadable(path);
    if (!error.empty()) {
        send_reply(client_fd, ipc::kStatusRejected, error);
        return;
    }

    on_add_(path);  // GUI work is queued; the reply does not wait for it.
    send_reply(client_fd, ipc::kStatusAccepted, "");
}

}  // namespace aiws
