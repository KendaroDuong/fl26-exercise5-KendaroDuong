# M5 Design Record

## 1. M4 Fixes

None of the M4 tests failed, but there were two places where the code used polling, which is not allowed by M5 constraint 3. In `IpcServer::serve()` and `read_exact()`, the code checked the stop flag every 100 ms. In `main.cpp`, a timer also checked the signal flag every 100 ms.

To fix this, I changed both to use pipes instead of polling. Now, `IpcServer::stop()` writes to a pipe to wake up the server thread. The SIGTERM handler also writes to a pipe, which is monitored by a `QSocketNotifier`. This allows the program to respond without repeatedly checking a flag.

## 2. Threads and Shared Data

The application uses three threads:

- **GUI thread:** Handles `MainWindow`, the widgets, `Workspace`, the current `ProcessingCore`, and all `CorpusState` objects.
- **Build worker:** Runs `BuildWorker::run()` on a single `std::thread`. The thread starts when `MainWindow` is created and is stopped and joined when the window is destroyed.
- **IPC thread:** Runs `IpcServer::serve()` and is joined when `IpcServer::stop()` is called.

| Shared Data | Threads | How It Is Protected |
|---|---|---|
| `pending_`, `wanted_id_`, `stop_`, `finished_` | GUI and worker | All access is protected by `BuildWorker::mutex_`. |
| Pipe file descriptors | GUI and worker / IPC | Created before the threads start and closed after they join. Data is passed through the pipes. |
| Remote-add path | IPC and GUI | The path is copied into a queued `invokeMethod()` call so it can be handled on the GUI thread. |

Each `BuildRequest` contains its own copy of the workspace, so the worker does not need to access the GUI's workspace directly. Once the build finishes, the result is moved back to the GUI thread.

The worker only holds the mutex when it needs to access shared data or wait for a request. I also made sure that when a new build is submitted, any completed result from an older build is cleared so it cannot be used.

## 3. States

The application uses five states to keep track of the corpus and determine what actions are allowed.

| State | Meaning | Build Requested | Workspace Changed | Reset | Build Finished/Failed |
|---|---|---|---|---|---|
| `NotBuiltState` | No corpus has been built, or the application was reset. | Building | Stays NotBuilt | NotBuilt | Ignored |
| `BuildingState(id)` | A build is currently wanted. | Ignored | Cancel → OutOfDate | Cancel → NotBuilt | Matching ID → Current / Failed; otherwise ignored |
| `CurrentState` | The corpus matches the workspace and can be used. | Building | OutOfDate | NotBuilt | Ignored |
| `OutOfDateState` | The workspace has changed since the build. | Building | Stays OutOfDate | NotBuilt | Ignored |
| `FailedState` | The last build failed. | Building | OutOfDate | NotBuilt | Ignored |

Each state handles an event and returns the next state. If nothing needs to change, it returns `nullptr`. `MainWindow::change_state()` then updates the current state and calls `enter()`.

When the application exits, `MainWindow` calls `BuildWorker::stop()` regardless of the current state. This makes sure the worker thread is stopped and joined before the application closes.

## 4. Trace: Reset During a Long Build

1. The user presses Reset, which calls `reset_application()`. This calls `BuildingState::reset()`, followed by `MainWindow::cancel_build()` and `BuildWorker::cancel()`. The worker locks the mutex, sets `wanted_id_` to 0, clears `pending_`, and calls `notify_one()`.

2. If the worker is waiting inside `build()` using `wake_.wait_for()`, the notification wakes it up. Since `unwanted(id)` is now true, the worker stops the current build and returns without a result.

3. The worker returns to `run()`. Since there is no completed result to save, it goes back to waiting for another request.

4. The GUI changes to `NotBuiltState` and displays "Corpus: Not built". If the user adds a document and presses Build again, `start_build()` submits a new request and immediately wakes the worker.

This makes sure that resetting during a long build does not force the user to wait for the old build to finish before starting another one.

## 5. Design Decisions

**1. Build requested during another build**

I decided not to allow another build to start while one is already running for the current workspace. `BuildingState::accepts_build_request()` disables the Build Corpus button, and `BuildingState::build_requested()` ignores any additional requests. There is no reason to start another build when one for the same workspace is already in progress.

**2. Remote add during a build**

A remote add is handled the same way as adding a document through the GUI. `remote_add_document()` calls `add_document_file()`, which then calls `BuildingState::workspace_changed()`.

This cancels the current build because the workspace has changed and the old build would no longer match it. The new document is added to the workspace, and the status changes to "Corpus: Out of date (Build Corpus required)". The Build Corpus button is also enabled so the user can start another build.

**3. Shared data between threads**

I kept `pending_`, `finished_`, `wanted_id_`, and `stop_` because each one has a different purpose.

- `pending_` holds the next build request for the worker.
- `finished_` holds the completed result until the GUI collects it.
- `wanted_id_` tells the worker whether the current build is still needed.
- `stop_` tells the worker when the application is shutting down.

These variables allow the GUI and worker to communicate without accessing each other's data directly. Each is needed to handle a different part of the build process.

## 6. Fault Switches

I included two fault switches to make sure the tests can detect problems with cancellation and shutdown.

| Macro | What It Changes | Test That Detects It |
|---|---|---|
| `AIWS_FAULT_NO_CANCEL` | Changes `BuildWorker::unwanted()` so it ignores `wanted_id_`, preventing normal cancellation. | `reset_during_long_build_does_not_delay_next_build` and `add_during_long_build_does_not_delay_next_build` |
| `AIWS_FAULT_SILENT_SHUTDOWN` | Removes `notify_one()` from `BuildWorker::stop()`, so an idle worker may never wake up to exit. | `idle_application_exits_promptly` |

The first fault checks that an old build can be canceled without delaying a new one. The second checks that the worker can shut down properly even when it is waiting for work.

## 7. AI-Use Disclosure

I used Claude to help create an outline for the worker, state classes, tests, and this design record. I used that outline as a starting point and then developed my own code and implementations based on my understanding of the requirements. The final code is my own work. I also reviewed and tested the implementation myself to make sure it worked correctly and that I understood how each part functions.
