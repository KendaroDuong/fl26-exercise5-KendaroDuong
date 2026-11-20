#pragma once

#include "aiws/processing_core.hpp"
#include "aiws/workspace.hpp"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace aiws {

/** @brief Makes the ProcessingCore a build uses. Runs on the worker thread. */
using CoreFactory = std::function<ProcessingCore()>;

/** @brief Everything the worker needs for one build. Moved in, never shared. */
struct BuildRequest {
    std::uint64_t id{0};
    Workspace snapshot;           // A private copy of the GUI's workspace.
    int delay_ms_per_document{0};
    CoreFactory make_core;
};

/** @brief What a build that ran to the end hands back to the GUI thread. */
struct BuildOutcome {
    std::uint64_t id{0};
    bool succeeded{false};
    ProcessingCore core;          // Valid only if succeeded.
    std::string error;            // Set only if !succeeded.
    std::thread::id built_on;     // The thread that ran the build.
};

// One long-lived worker thread. The GUI submits a copied workspace and
// receives completed builds through a pipe watched by QSocketNotifier.
// All shared state is protected by mutex_.
class BuildWorker {
public:
    /** @brief Starts the worker thread. Throws std::system_error if the pipe fails. */
    BuildWorker();
    ~BuildWorker();

    BuildWorker(const BuildWorker&) = delete;
    BuildWorker& operator=(const BuildWorker&) = delete;

    /** @brief GUI: replaces any pending request; a running older build becomes unwanted. */
    void submit(BuildRequest request);

    /** @brief GUI: drops the pending request and makes the running build unwanted. */
    void cancel();

    /** @brief GUI: wakes the worker, makes it return, and joins it. Idempotent. */
    void stop();

    /** @brief Becomes readable when a wanted build has finished. */
    int finished_fd() const noexcept { return wake_gui_read_fd_; }

    /** @brief GUI: the newest finished build, if any. Clears the pipe. */
    std::optional<BuildOutcome> take_finished();

private:
    void run();
    std::optional<BuildOutcome> build(BuildRequest& request);
    bool unwanted(std::uint64_t id) const;  // Caller holds mutex_.

    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<BuildRequest> pending_;  // Guarded by mutex_.
    std::uint64_t wanted_id_{0};           // Guarded by mutex_. 0 = no build wanted.
    bool stop_{false};                     // Guarded by mutex_.
    std::optional<BuildOutcome> finished_; // Guarded by mutex_. Newest wins.

    int wake_gui_read_fd_{-1};   // Set before the thread starts, closed after join.
    int wake_gui_write_fd_{-1};  // Ditto. Only the worker writes to it.

    std::thread thread_;  // Declared last: starts after everything above exists.
};

}  // namespace aiws
