#include "aiws/gui/main_window.hpp"

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QPushButton>
#include <QSocketNotifier>
#include <QVBoxLayout>
#include <QWidget>

#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aiws/document.hpp"
#include "aiws/gui/context_widget.hpp"
#include "aiws/gui/search_widget.hpp"
#include "aiws/gui/term_inspector_widget.hpp"
#include "aiws/gui/workspace_widget.hpp"

namespace aiws::gui {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle(tr("AI Workspace"));

    auto* central = new QWidget(this);
    auto* header = new QLabel(tr("AI Workspace"), central);
    header->setStyleSheet(
        "QLabel { background-color: #2c4a7a; color: white;"
        " font-size: 16pt; font-weight: bold; padding: 8px; }");

    workspace_widget_ = new WorkspaceWidget(central);
    search_widget_ = new SearchWidget(central);
    term_inspector_widget_ = new TermInspectorWidget(central);
    context_widget_ = new ContextWidget(central);
    status_label_ = new QLabel(tr("Ready."), central);

    reset_button_ = new QPushButton(tr("Reset"), central);
    reset_button_->setStyleSheet(
        "QPushButton { background-color: #f0c040;"
        " color: black; padding: 6px 18px; }");

    exit_button_ = new QPushButton(tr("Exit"), central);
    exit_button_->setStyleSheet(
        "QPushButton { background-color: #c0392b;"
        " color: white; padding: 6px 18px; }");

    auto* analysis_layout = new QHBoxLayout;
    analysis_layout->addWidget(term_inspector_widget_, 1);
    analysis_layout->addWidget(context_widget_, 2);

    auto* bottom_layout = new QHBoxLayout;
    bottom_layout->addWidget(status_label_, 1);
    bottom_layout->addWidget(reset_button_);
    bottom_layout->addWidget(exit_button_);

    auto* layout = new QVBoxLayout(central);
    layout->addWidget(header);
    layout->addWidget(workspace_widget_, 3);
    layout->addWidget(search_widget_, 4);
    layout->addLayout(analysis_layout, 3);
    layout->addLayout(bottom_layout);

    setCentralWidget(central);
    resize(900, 900);

    connect(workspace_widget_, &WorkspaceWidget::add_document_requested,
            this, &MainWindow::add_document);
    connect(workspace_widget_, &WorkspaceWidget::build_corpus_requested,
            this, &MainWindow::build_corpus);
    connect(workspace_widget_, &WorkspaceWidget::document_selected,
            this, &MainWindow::document_selected);

    connect(search_widget_, &SearchWidget::search_requested,
            this, &MainWindow::search_requested);
    connect(search_widget_, &SearchWidget::query_changed,
            this, &MainWindow::query_changed);
    connect(search_widget_, &SearchWidget::source_document_requested,
            this, &MainWindow::source_document_requested);
    connect(search_widget_, &SearchWidget::selected_chunk_changed,
            term_inspector_widget_,
            &TermInspectorWidget::set_selected_chunk);

    connect(term_inspector_widget_,
            &TermInspectorWidget::term_inspection_requested,
            this, &MainWindow::term_inspection_requested);
    connect(context_widget_, &ContextWidget::build_context_requested,
            this, &MainWindow::build_context_requested);

    connect(reset_button_, &QPushButton::clicked,
            this, &MainWindow::reset_application);
    connect(exit_button_, &QPushButton::clicked,
            this, &MainWindow::exit_application);

    // The one build thread (R1), alive until ~MainWindow(). When a wanted
    // build finishes, the worker makes finished_fd() readable, and the event
    // loop runs collect_finished_build() here on the GUI thread.
    worker_ = std::make_unique<BuildWorker>();
    build_done_notifier_ = std::make_unique<QSocketNotifier>(
        worker_->finished_fd(), QSocketNotifier::Read);
    connect(build_done_notifier_.get(), &QSocketNotifier::activated,
            this, &MainWindow::collect_finished_build);

    change_state(std::make_unique<NotBuiltState>());
}

MainWindow::~MainWindow() {
    // Stop serving first: nothing may queue new work while the window goes away.
    ipc_server_.reset();

    // Exit, in any state: wake the worker, let it drop any build, and join it.
    build_done_notifier_.reset();
    worker_->stop();
}

void MainWindow::set_exit_callback(ExitCallback callback) noexcept {
    exit_callback_ = callback;
}

bool MainWindow::add_document_file(const QString& path) {
    const QFileInfo info(path);
    const std::string id = info.absoluteFilePath().toStdString();

    for (const auto& document : workspace_.documents()) {
        if (document.id() == id) {
            show_message(tr("%1 is already in the workspace.").arg(info.fileName()));
            return true;
        }
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        show_message(tr("Could not open %1: %2")
                         .arg(info.fileName(), file.errorString()));
        return false;
    }

    const auto text = QString::fromUtf8(file.readAll());
    workspace_.add_document(Document(id, info.fileName().toStdString(),
                                     text.toStdString()));
    workspace_widget_->add_document(info.fileName());
    change_state(state_->workspace_changed(*this));
    show_message(tr("Added %1.").arg(info.fileName()));
    return true;
}

void MainWindow::set_build_delay_ms(int ms_per_document) {
    build_delay_ms_ = ms_per_document;
}

void MainWindow::set_core_factory(CoreFactory factory) {
    core_factory_ = std::move(factory);
}

bool MainWindow::build_in_progress() const noexcept {
    return state_->build_running();
}

std::thread::id MainWindow::last_build_thread() const noexcept {
    return last_build_thread_;
}

QString MainWindow::corpus_state_name() const {
    return state_->name();
}

bool MainWindow::start_ipc_server(const QString& socket_path, QString* error) {
    auto server = std::make_unique<IpcServer>(
        socket_path.toStdString(),
        [this](const std::string& path) {
            // Runs on the server thread. A queued call copies the path and
            // returns at once; the slot runs later on the GUI thread.
            QMetaObject::invokeMethod(
                this, "remote_add_document", Qt::QueuedConnection,
                Q_ARG(QString, QString::fromStdString(path)));
        });

    std::string message;
    if (!server->start(message)) {
        if (error != nullptr) {
            *error = QString::fromStdString(message);
        }
        return false;
    }
    ipc_server_ = std::move(server);
    return true;
}

void MainWindow::remote_add_document(const QString& path) {
    add_document_file(path);
}

void MainWindow::add_document() {
    QString path = QFileDialog::getOpenFileName(
        this, tr("Add Document"), QString(),
        tr("Text files (*.txt);;All files (*)"));

    if (!path.isEmpty()) {
        add_document_file(path);
    }
}

void MainWindow::build_corpus() {
    if (workspace_.documents().empty()) {
        return;  // Nothing to build; the button is disabled anyway.
    }
    change_state(state_->build_requested(*this));
}

void MainWindow::collect_finished_build() {
    auto result = worker_->take_finished();
    if (!result) {
        return;
    }

    last_build_thread_ = result->built_on;
    if (result->succeeded) {
        change_state(state_->build_succeeded(*this, result->id, result->core));
    } else {
        change_state(state_->build_failed(
            *this, result->id, QString::fromStdString(result->error)));
    }
}

void MainWindow::change_state(std::unique_ptr<CorpusState> next) {
    if (next) {
        state_ = std::move(next);
        state_->enter(*this);
    }
    update_action_states();
}

// ---- CorpusContext -------------------------------------------------------

std::uint64_t MainWindow::start_build() {
    const auto id = ++last_build_id_;
    worker_->submit(BuildRequest{id, workspace_, build_delay_ms_, core_factory_});
    return id;
}

void MainWindow::cancel_build() {
    worker_->cancel();
}

void MainWindow::install_corpus(ProcessingCore core) {
    processing_core_ = std::move(core);
    status_label_->setText(
        tr("Corpus built from %1 document(s).")
            .arg(static_cast<qulonglong>(workspace_.documents().size())));
}

std::size_t MainWindow::corpus_chunk_count() const {
    return processing_core_.chunk_count();
}

void MainWindow::show_corpus_status(const QString& text) {
    workspace_widget_->set_corpus_status(text);
}

void MainWindow::show_message(const QString& text) {
    status_label_->setText(text);
}

void MainWindow::document_selected(int row) {
    const auto& documents = workspace_.documents();

    if (row < 0 ||
        static_cast<std::size_t>(row) >= documents.size()) {
        workspace_widget_->clear_selected_document();
        return;
    }

    const Document& document = documents[row];

    workspace_widget_->show_selected_document(
        QString::fromStdString(document.title()),
        QString::fromStdString(document.text()));
}

void MainWindow::search_requested(const QString& query, int k) {
    QString text = query.trimmed();

    if (!corpus_is_current() || text.isEmpty()) {
        return;
    }

    std::vector<SearchResult> results;

    try {
        results = processing_core_.search(text.toStdString(), k);
    } catch (const std::exception& error) {
        status_label_->setText(
            tr("Search failed: %1")
                .arg(QString::fromStdString(error.what())));
        return;
    }

    auto count = results.size();

    search_widget_->set_results(std::move(results));
    context_widget_->clear_context();

    status_label_->setText(
        tr("Search returned %1 result(s).")
            .arg(static_cast<qulonglong>(count)));

    update_action_states();
}

void MainWindow::query_changed(const QString&) {
    update_action_states();
}

void MainWindow::build_context_requested(int token_budget) {
    QString query = search_widget_->query().trimmed();

    if (!corpus_is_current() || query.isEmpty()) {
        return;
    }

    std::vector<ContextItem> items;

    try {
        items = processing_core_.build_context(
            query.toStdString(),
            search_widget_->result_count(),
            static_cast<std::size_t>(token_budget));
    } catch (const std::exception& error) {
        status_label_->setText(
            tr("Build Context failed: %1")
                .arg(QString::fromStdString(error.what())));
        return;
    }

    context_widget_->set_context(items);

    status_label_->setText(
        tr("Context built: %1 item(s), budget %2 tokens.")
            .arg(static_cast<qulonglong>(items.size()))
            .arg(token_budget));
}

void MainWindow::source_document_requested(
    const QString& document_id) {
    const auto& documents = workspace_.documents();

    for (std::size_t i = 0; i < documents.size(); ++i) {
        if (documents[i].id() == document_id.toStdString()) {
            int row = static_cast<int>(i);

            workspace_widget_->select_document(row);
            document_selected(row);

            status_label_->setText(
                tr("Showing source document %1.")
                    .arg(QString::fromStdString(
                        documents[i].title())));
            return;
        }
    }

    status_label_->setText(
        tr("Source document is not in the workspace."));
}

void MainWindow::term_inspection_requested(
    const QString& term, const QString& chunk_id) {
    QString text = term.trimmed();

    if (!corpus_is_current() || text.isEmpty()) {
        return;
    }

    try {
        std::string word = text.toStdString();

        std::size_t df =
            processing_core_.document_frequency(word);

        bool has_chunk = !chunk_id.isEmpty();
        std::size_t tf = 0;

        if (has_chunk) {
            tf = processing_core_.term_frequency(
                word, chunk_id.toStdString());
        }

        term_inspector_widget_->set_statistics(df, has_chunk, tf);

        status_label_->setText(
            tr("Inspected term \"%1\".").arg(text));
    } catch (const std::exception& error) {
        term_inspector_widget_->clear_statistics();

        status_label_->setText(
            tr("Inspect failed: %1")
                .arg(QString::fromStdString(error.what())));
    }
}

void MainWindow::reset_application() {
    workspace_.clear();
    processing_core_ = ProcessingCore{};

    workspace_widget_->reset();
    search_widget_->reset();
    term_inspector_widget_->reset();
    context_widget_->reset();

    change_state(state_->reset(*this));
    status_label_->setText(tr("Application reset."));
}

void MainWindow::exit_application() {
    if (exit_callback_ != nullptr) {
        exit_callback_(this);
    }
}

bool MainWindow::corpus_is_current() const noexcept {
    return state_->corpus_is_current();
}

void MainWindow::update_action_states() {
    bool has_documents = !workspace_.documents().empty();
    bool has_query = search_widget_->has_query();
    bool current = corpus_is_current();

    workspace_widget_->set_build_corpus_enabled(
        has_documents && state_->accepts_build_request());
    search_widget_->set_search_enabled(current && has_query);
    term_inspector_widget_->set_inspection_enabled(current);
    context_widget_->set_build_context_enabled(current && has_query);
}

void MainWindow::clear_processing_outputs() {
    search_widget_->clear_results();
    context_widget_->clear_context();
    term_inspector_widget_->clear_statistics();
}

}  // namespace aiws::gui
