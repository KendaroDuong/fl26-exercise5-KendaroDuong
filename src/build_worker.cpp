#include "aiws/build_worker.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <exception>
#include <system_error>
#include <utility>

namespace aiws {

BuildWorker::BuildWorker() {
    int pipe_fds[2];
    if (::pipe2(pipe_fds, O_NONBLOCK | O_CLOEXEC) == -1) {
        throw std::system_error(errno, std::generic_category(), "pipe2");
    }

    wake_gui_read_fd_ = pipe_fds[0];
    wake_gui_write_fd_ = pipe_fds[1];
    try {
        thread_ = std::thread(&BuildWorker::run, this);
    } catch (...) {
        ::close(wake_gui_read_fd_);
        ::close(wake_gui_write_fd_);
        throw;
    }
}

BuildWorker::~BuildWorker() {
    stop();
    ::close(wake_gui_read_fd_);
    ::close(wake_gui_write_fd_);
}

void BuildWorker::submit(BuildRequest request) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wanted_id_ = request.id;
        pending_ = std::move(request);
        finished_.reset();
    }
    wake_.notify_one();
}

void BuildWorker::cancel() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wanted_id_ = 0;
        pending_.reset();
        finished_.reset();
    }
    wake_.notify_one();
}

void BuildWorker::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        wanted_id_ = 0;
        pending_.reset();
    }
#ifndef AIWS_FAULT_SILENT_SHUTDOWN
    wake_.notify_one();
#endif
    if (thread_.joinable()) {
        thread_.join();
    }
}

std::optional<BuildOutcome> BuildWorker::take_finished() {
    char bytes[64];
    while (::read(wake_gui_read_fd_, bytes, sizeof(bytes)) > 0) {
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto result = std::move(finished_);
    finished_.reset();
    return result;
}

bool BuildWorker::unwanted(std::uint64_t id) const {
#ifdef AIWS_FAULT_NO_CANCEL
    (void)id;
    return stop_;
#else
    return stop_ || wanted_id_ != id;
#endif
}

void BuildWorker::run() {
    std::unique_lock<std::mutex> lock(mutex_);

    while (true) {
        wake_.wait(lock, [this] { return stop_ || pending_.has_value(); });
        if (stop_) {
            break;
        }

        BuildRequest request = std::move(*pending_);
        pending_.reset();

        lock.unlock();
        auto result = build(request);
        lock.lock();

        if (!result || unwanted(request.id)) {
            continue;
        }

        finished_ = std::move(result);
        const char signal = 1;
        (void)::write(wake_gui_write_fd_, &signal, 1);
    }
}

std::optional<BuildOutcome> BuildWorker::build(BuildRequest& request) {
    const auto delay = std::chrono::milliseconds(request.delay_ms_per_document);

    // Wait for each document, but wake immediately on cancel or shutdown.
    for (const auto& document : request.snapshot.documents()) {
        (void)document;
        std::unique_lock<std::mutex> lock(mutex_);
        if (wake_.wait_for(lock, delay,
                           [this, &request] { return unwanted(request.id); })) {
            return std::nullopt;
        }
    }

    BuildOutcome result;
    result.id = request.id;
    result.built_on = std::this_thread::get_id();

    try {
        result.core = request.make_core ? request.make_core() : ProcessingCore{};
        result.core.rebuild(request.snapshot);
        result.succeeded = true;
    } catch (const std::exception& e) {
        result.error = e.what();
    } catch (...) {
        result.error = "unknown error";
    }
    return result;
}

}  // namespace aiws
