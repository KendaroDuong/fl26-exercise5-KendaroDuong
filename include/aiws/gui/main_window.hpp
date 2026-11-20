#pragma once

#include <QMainWindow>

#include "aiws/build_worker.hpp"
#include "aiws/gui/corpus_state.hpp"
#include "aiws/ipc_server.hpp"
#include "aiws/processing_core.hpp"
#include "aiws/workspace.hpp"

#include <cstdint>
#include <memory>
#include <thread>

class QLabel;
class QPushButton;
class QSocketNotifier;
class QString;
class QWidget;

namespace aiws::gui {

class ContextWidget;
class SearchWidget;
class TermInspectorWidget;
class WorkspaceWidget;

/**
 * @brief Main application window and coordinator for the M3 AI Workspace GUI.
 *
 * MainWindow owns the application Workspace and ProcessingCore, composes the
 * major GUI components, and coordinates application-wide state changes and
 * backend operations in response to component events.
 */
class MainWindow final : public QMainWindow, private CorpusContext {
    Q_OBJECT

public:
    /**
     * @brief Function-pointer type used for the application Exit action.
     *
     * The callback receives the main window as a parent for any exit-related
     * interaction. In M3, the registered callback asks the user to confirm the
     * exit. Declining the confirmation leaves the running application and its
     * current state unchanged.
     */
    using ExitCallback = void (*)(QWidget* parent);

    /**
     * @brief Constructs the main AI Workspace application window.
     * @param parent Optional Qt parent widget.
     */
    explicit MainWindow(QWidget* parent = nullptr);

    /** @brief Stops the IPC server, then stops and joins the build worker. */
    ~MainWindow() override;

    /**
     * @brief Registers the callback invoked when the user requests Exit.
     * @param callback Function pointer to invoke for the Exit action. A null
     * callback means that an Exit request has no registered callback to invoke.
     */
    void set_exit_callback(ExitCallback callback) noexcept;

    // ---- M4 required public API (see specification, Section 5) ----------

    /**
     * @brief Adds the file at path exactly as the Add Document action does.
     * @return false, changing nothing, if the file cannot be read.
     */
    bool add_document_file(const QString& path);

    /**
     * @brief Sets the extra build time per document for builds started after this call.
     * @param ms_per_document Delay in milliseconds (the --build-delay-ms value).
     */
    void set_build_delay_ms(int ms_per_document);

    /** @brief Replaces how builds make their ProcessingCore. Lets tests force a failure. */
    void set_core_factory(CoreFactory factory);

    /** @brief True while a build for the present workspace is pending or running. */
    bool build_in_progress() const noexcept;

    /** @brief The thread that ran the last build that finished (for tests of R1). */
    std::thread::id last_build_thread() const noexcept;

    /** @brief Name of the current corpus state, for display and tests only. */
    QString corpus_state_name() const;

    /**
     * @brief Starts serving Remote Add requests on socket_path.
     * @return false, with error set if it is not null, if the socket is unavailable.
     */
    bool start_ipc_server(const QString& socket_path, QString* error = nullptr);

    // Everything below this line is the M3 design. In M4 it is yours to
    // change: add, remove, or redesign private members, slots, and helpers.

private slots:
    /** @brief Handles a request to select and add a document to the workspace. */
    void add_document();

    /** @brief Rebuilds the processing corpus from the current non-empty workspace. */
    void build_corpus();

    /**
     * @brief Updates the selected-document display for the requested workspace row.
     * @param row Zero-based document row, or a negative value when no row is selected.
     */
    void document_selected(int row);

    /**
     * @brief Performs a search of the current corpus and presents the returned results.
     * @param query Query text to search for.
     * @param k Maximum number of results requested.
     */
    void search_requested(const QString& query, int k);

    /**
     * @brief Responds to a query edit by updating application action availability.
     * @param query Updated query text.
     */
    void query_changed(const QString& query);

    /**
     * @brief Builds and displays context for the current query and search settings.
     * @param token_budget Maximum token budget requested for the context.
     */
    void build_context_requested(int token_budget);
    /**
     * @brief Selects and displays the workspace document identified by a search result.
     * @param document_id Identifier of the source document to reveal.
     */
    void source_document_requested(const QString& document_id);

    /**
     * @brief Queries and presents term statistics for the current corpus.
     * @param term Term to inspect.
     * @param chunk_id Selected result chunk identifier; may be empty, in which
     * case document frequency is still available but chunk frequency is not.
     */
    void term_inspection_requested(const QString& term, const QString& chunk_id);

    /**
     * @brief Restores the application and its major GUI components to initial state.
     */
    void reset_application();

    /**
     * @brief Invokes the registered Exit callback for the current exit request.
     */
    void exit_application();

    /** @brief GUI-thread end of a Remote Add request, reached by a queued call. */
    void remote_add_document(const QString& path);

    /** @brief Runs when the worker signals a finished build; feeds it to the state. */
    void collect_finished_build();

private:
    /** @brief Makes next the current state, unless a handler returned nullptr. */
    void change_state(std::unique_ptr<CorpusState> next);

    /** @brief True only when the corpus reflects the present workspace. */
    bool corpus_is_current() const noexcept;

    /**
     * @brief Updates availability of actions that depend on current application state.
     */
    void update_action_states();

    // ---- CorpusContext: what the states may ask for ------------------
    std::uint64_t start_build() override;
    void cancel_build() override;
    void install_corpus(ProcessingCore core) override;
    std::size_t corpus_chunk_count() const override;
    void show_corpus_status(const QString& text) override;
    void show_message(const QString& text) override;
    void clear_processing_outputs() override;

    Workspace workspace_;
    ProcessingCore processing_core_;
    std::unique_ptr<CorpusState> state_;  // Never null after construction.
    std::uint64_t last_build_id_{0};      // GUI thread only.
    std::thread::id last_build_thread_;   // GUI thread only.
    int build_delay_ms_{0};
    CoreFactory core_factory_;
    std::unique_ptr<BuildWorker> worker_;
    std::unique_ptr<QSocketNotifier> build_done_notifier_;  // Destroyed before worker_.
    std::unique_ptr<IpcServer> ipc_server_;
    ExitCallback exit_callback_{nullptr};

    WorkspaceWidget* workspace_widget_{nullptr};
    SearchWidget* search_widget_{nullptr};
    TermInspectorWidget* term_inspector_widget_{nullptr};
    ContextWidget* context_widget_{nullptr};
    QLabel* status_label_{nullptr};
    QPushButton* reset_button_{nullptr};
    QPushButton* exit_button_{nullptr};
};

}  // namespace aiws::gui
