#include "aiws/chunker.hpp"
#include "aiws/context_builder.hpp"
#include "aiws/gui/main_window.hpp"
#include "aiws/ipc_protocol.hpp"
#include "aiws/retrieval_engine.hpp"

#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QProcess>
#include <QSet>
#include <QPushButton>
#include <QTemporaryDir>
#include <QtTest>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

using aiws::gui::MainWindow;

namespace {

constexpr int kWaitMs = 5000;

class FailingChunker final : public aiws::ChunkingStrategy {
public:
    std::vector<aiws::Chunk> chunk(const aiws::Document&, std::size_t) const override {
        throw std::runtime_error("forced chunking failure");
    }
};

aiws::ProcessingCore make_failing_core() {
    return aiws::ProcessingCore(std::make_unique<FailingChunker>(),
                                std::make_unique<aiws::RetrievalEngine>(),
                                std::make_unique<aiws::ContextBuilder>());
}

QPushButton* button(MainWindow& window, const QString& text) {
    for (QPushButton* candidate : window.findChildren<QPushButton*>()) {
        if (candidate->text() == text) {
            return candidate;
        }
    }
    return nullptr;
}

// The label a user reads: "Corpus: Not built", "Corpus: 3 chunks", and so on.
QString corpus_status(MainWindow& window) {
    for (QLabel* label : window.findChildren<QLabel*>()) {
        if (label->text().startsWith("Corpus:")) {
            return label->text();
        }
    }
    return {};
}

bool shows_chunks(MainWindow& window) {
    return corpus_status(window).endsWith("chunks");
}

int document_count(MainWindow& window) {
    return window.findChild<QListWidget*>()->count();
}

QString write_file(const QTemporaryDir& dir, const QString& name) {
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return {};
    }
    file.write("alpha beta gamma delta epsilon zeta eta theta\n");
    return path;
}

struct SendResult {
    int exit_code;
    QByteArray error;
};

SendResult run_send(const QString& socket, const QString& file,
                    const QString& working_directory = QString()) {
    QProcess process;
    if (!working_directory.isEmpty()) {
        process.setWorkingDirectory(working_directory);
    }
    process.start(AIWS_SEND_PATH, {"--socket", socket, "add", file});
    // Blocks the GUI thread on purpose: aiws_send must still get its reply,
    // which proves the reply does not wait for the GUI thread.
    process.waitForFinished(kWaitMs + 1000);
    return {process.exitCode(), process.readAllStandardError()};
}

}  // namespace

class StudentTests final : public QObject {
    Q_OBJECT

private slots:
    // M5. The process test runs first: it fails fast, where an in-process
    // hang could only end at the CTest timeout.
    void idle_application_exits_promptly();
    void every_build_runs_on_one_worker_thread();
    void reset_during_long_build_does_not_delay_next_build();
    void add_during_long_build_does_not_delay_next_build();
    void exiting_during_long_build_is_prompt();
    void failed_build_then_add_is_out_of_date();
    void build_is_ignored_while_building();

    // M4 and CR-1, rerun.
    void build_runs_in_background_and_becomes_current();
    void gui_stays_usable_during_a_build();
    void add_during_build_discards_stale_result();
    void reset_during_build_discards_stale_result();
    void build_clicked_right_after_add_reflects_all_documents();
    void failed_build_is_reported_and_app_stays_usable();
    void destroying_window_during_build_is_prompt();
    void repeated_builds_all_finish();

    void add_file_rejects_unreadable_path();
    void remote_add_relative_path();
    void remote_add_during_build_invalidates_it();
    void remote_add_special_paths_data();
    void remote_add_special_paths();
    void unreadable_file_exits_1();
    void no_application_exits_2();
    void request_split_into_pieces_is_received();
    void socket_removed_within_500ms_of_exit();
};

// ---- M5 -------------------------------------------------------------------

// R4: Exit with no build running. Runs the real application and ends it with
// SIGTERM, the same quit path as the Exit button.
void StudentTests::idle_application_exits_promptly() {
    QTemporaryDir dir;
    const QString socket = dir.filePath("s.sock");
    QProcess app;
    app.start(AIWS_APP_PATH, {"--ipc-socket", socket});
    QVERIFY(app.waitForStarted(kWaitMs));
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(socket), kWaitMs);  // Fully up.

    QElapsedTimer timer;
    timer.start();
    app.terminate();
    const bool exited = app.waitForFinished(3000);
    if (!exited) {
        app.kill();
        app.waitForFinished(kWaitMs);
    }
    QVERIFY2(exited, "the application did not exit within 3 s of SIGTERM");
    QVERIFY(timer.elapsed() < 1000);
}

// R1: builds for different workspaces all run on the same thread, which is
// not the GUI thread.
void StudentTests::every_build_runs_on_one_worker_thread() {
    QTemporaryDir dir;
    MainWindow window;
    QSet<QString> threads;

    for (int i = 0; i < 5; ++i) {
        QVERIFY(window.add_document_file(write_file(dir, QString("d%1.txt").arg(i))));
        button(window, "Build Corpus")->click();
        QTRY_VERIFY_WITH_TIMEOUT(shows_chunks(window), kWaitMs);

        QVERIFY(window.last_build_thread() != std::thread::id());
        QVERIFY(window.last_build_thread() != std::this_thread::get_id());
        std::ostringstream id;
        id << window.last_build_thread();
        threads.insert(QString::fromStdString(id.str()));
    }
    QCOMPARE(threads.size(), 1);
}

// R3: Reset makes a long build useless. The next build must not wait for it.
void StudentTests::reset_during_long_build_does_not_delay_next_build() {
    QTemporaryDir dir;
    MainWindow window;
    for (const char* name : {"a.txt", "b.txt", "c.txt"}) {
        QVERIFY(window.add_document_file(write_file(dir, name)));
    }
    window.set_build_delay_ms(3000);  // A 9 s build.
    button(window, "Build Corpus")->click();
    QTest::qWait(100);                // Let the worker start it.

    button(window, "Reset")->click();
    QCOMPARE(corpus_status(window), QString("Corpus: Not built"));

    window.set_build_delay_ms(0);
    QVERIFY(window.add_document_file(write_file(dir, "d.txt")));
    QElapsedTimer timer;
    timer.start();
    button(window, "Build Corpus")->click();
    QTRY_VERIFY_WITH_TIMEOUT(shows_chunks(window), 2000);
    QVERIFY(timer.elapsed() < 2000);
}

// R3, and decision 2: a workspace change (here add_document_file) cancels.
void StudentTests::add_during_long_build_does_not_delay_next_build() {
    QTemporaryDir dir;
    MainWindow window;
    for (const char* name : {"a.txt", "b.txt", "c.txt"}) {
        QVERIFY(window.add_document_file(write_file(dir, name)));
    }
    window.set_build_delay_ms(3000);
    button(window, "Build Corpus")->click();
    QTest::qWait(100);

    QVERIFY(window.add_document_file(write_file(dir, "d.txt")));
    QCOMPARE(window.corpus_state_name(), QString("OutOfDate"));
    QVERIFY(!window.build_in_progress());
    QVERIFY(button(window, "Build Corpus")->isEnabled());

    window.set_build_delay_ms(0);
    button(window, "Build Corpus")->click();
    QTRY_VERIFY_WITH_TIMEOUT(shows_chunks(window), 2000);
}

// R4 during a build: the worker wakes from its delay and is joined at once.
void StudentTests::exiting_during_long_build_is_prompt() {
    QTemporaryDir dir;
    QElapsedTimer timer;
    {
        MainWindow window;
        QVERIFY(window.add_document_file(write_file(dir, "a.txt")));
        window.set_build_delay_ms(60000);
        button(window, "Build Corpus")->click();
        QTest::qWait(100);
        timer.start();
    }
    QVERIFY(timer.elapsed() < 1000);
}

// States: Failed --workspace change--> OutOfDate --build--> Building --> Current.
void StudentTests::failed_build_then_add_is_out_of_date() {
    QTemporaryDir dir;
    MainWindow window;
    QVERIFY(window.add_document_file(write_file(dir, "a.txt")));
    window.set_core_factory(make_failing_core);
    button(window, "Build Corpus")->click();
    QTRY_COMPARE_WITH_TIMEOUT(window.corpus_state_name(), QString("Failed"), kWaitMs);

    QVERIFY(window.add_document_file(write_file(dir, "b.txt")));
    QCOMPARE(window.corpus_state_name(), QString("OutOfDate"));
    QCOMPARE(corpus_status(window),
             QString("Corpus: Out of date (Build Corpus required)"));

    window.set_core_factory(nullptr);
    button(window, "Build Corpus")->click();
    QCOMPARE(window.corpus_state_name(), QString("Building"));
    QTRY_COMPARE_WITH_TIMEOUT(window.corpus_state_name(), QString("Current"), kWaitMs);
}

// Decision 1: Build Corpus is disabled while building, and the Building state
// ignores a request that arrives anyway.
void StudentTests::build_is_ignored_while_building() {
    QTemporaryDir dir;
    MainWindow window;
    QVERIFY(window.add_document_file(write_file(dir, "a.txt")));
    window.set_build_delay_ms(200);
    button(window, "Build Corpus")->click();
    QVERIFY(!button(window, "Build Corpus")->isEnabled());

    QMetaObject::invokeMethod(&window, "build_corpus");  // As if clicked anyway.
    QCOMPARE(window.corpus_state_name(), QString("Building"));
    QTRY_VERIFY_WITH_TIMEOUT(shows_chunks(window), kWaitMs);
}

// ---- M4 and CR-1 ---------------------------------------------------------

void StudentTests::build_runs_in_background_and_becomes_current() {
    QTemporaryDir dir;
    MainWindow window;
    QVERIFY(window.add_document_file(write_file(dir, "a.txt")));

    button(window, "Build Corpus")->click();
    QCOMPARE(corpus_status(window), QString("Corpus: Building..."));
    QTRY_VERIFY_WITH_TIMEOUT(shows_chunks(window), kWaitMs);
}

void StudentTests::gui_stays_usable_during_a_build() {
    QTemporaryDir dir;
    MainWindow window;
    QVERIFY(window.add_document_file(write_file(dir, "a.txt")));
    window.set_build_delay_ms(60000);

    QElapsedTimer timer;
    timer.start();
    button(window, "Build Corpus")->click();
    QVERIFY(timer.elapsed() < 2000);  // A blocking build would take a minute.
    QVERIFY(window.build_in_progress());
    QVERIFY(!button(window, "Build Corpus")->isEnabled());

    QVERIFY(window.add_document_file(write_file(dir, "b.txt")));
    QCOMPARE(document_count(window), 2);
}

void StudentTests::add_during_build_discards_stale_result() {
    QTemporaryDir dir;
    MainWindow window;
    QVERIFY(window.add_document_file(write_file(dir, "a.txt")));
    window.set_build_delay_ms(100);

    button(window, "Build Corpus")->click();
    QVERIFY(window.add_document_file(write_file(dir, "b.txt")));

    QTRY_VERIFY_WITH_TIMEOUT(!window.build_in_progress(), kWaitMs);
    QVERIFY(!shows_chunks(window));
    QVERIFY(button(window, "Build Corpus")->isEnabled());
}

void StudentTests::reset_during_build_discards_stale_result() {
    QTemporaryDir dir;
    MainWindow window;
    QVERIFY(window.add_document_file(write_file(dir, "a.txt")));
    window.set_build_delay_ms(100);

    button(window, "Build Corpus")->click();
    button(window, "Reset")->click();

    QTRY_VERIFY_WITH_TIMEOUT(!window.build_in_progress(), kWaitMs);
    QCOMPARE(corpus_status(window), QString("Corpus: Not built"));
    QCOMPARE(document_count(window), 0);
}

void StudentTests::build_clicked_right_after_add_reflects_all_documents() {
    QTemporaryDir dir;

    MainWindow expected;
    QVERIFY(expected.add_document_file(write_file(dir, "a.txt")));
    QVERIFY(expected.add_document_file(write_file(dir, "b.txt")));
    button(expected, "Build Corpus")->click();
    QTRY_VERIFY_WITH_TIMEOUT(shows_chunks(expected), kWaitMs);

    MainWindow window;
    QVERIFY(window.add_document_file(write_file(dir, "a.txt")));
    window.set_build_delay_ms(50);
    button(window, "Build Corpus")->click();
    QVERIFY(window.add_document_file(write_file(dir, "b.txt")));
    button(window, "Build Corpus")->click();  // The old worker is still finishing.

    QTRY_VERIFY_WITH_TIMEOUT(shows_chunks(window), kWaitMs);
    QCOMPARE(corpus_status(window), corpus_status(expected));
}

void StudentTests::failed_build_is_reported_and_app_stays_usable() {
    QTemporaryDir dir;
    MainWindow window;
    QVERIFY(window.add_document_file(write_file(dir, "a.txt")));

    window.set_core_factory(make_failing_core);
    button(window, "Build Corpus")->click();
    QTRY_COMPARE_WITH_TIMEOUT(corpus_status(window),
                              QString("Corpus: Build failed"), kWaitMs);

    window.set_core_factory(nullptr);
    QVERIFY(button(window, "Build Corpus")->isEnabled());
    button(window, "Build Corpus")->click();
    QTRY_VERIFY_WITH_TIMEOUT(shows_chunks(window), kWaitMs);
}

void StudentTests::destroying_window_during_build_is_prompt() {
    QTemporaryDir dir;
    QElapsedTimer timer;
    {
        MainWindow window;
        QVERIFY(window.add_document_file(write_file(dir, "a.txt")));
        window.set_build_delay_ms(60000);
        button(window, "Build Corpus")->click();
        timer.start();
    }
    QVERIFY(timer.elapsed() < 2000);
}

void StudentTests::repeated_builds_all_finish() {
    QTemporaryDir dir;
    MainWindow window;
    QVERIFY(window.add_document_file(write_file(dir, "a.txt")));

    for (int i = 0; i < 20; ++i) {
        button(window, "Build Corpus")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!window.build_in_progress(), kWaitMs);
        QVERIFY(shows_chunks(window));
    }
}

void StudentTests::add_file_rejects_unreadable_path() {
    QTemporaryDir dir;
    MainWindow window;
    QVERIFY(!window.add_document_file(dir.filePath("missing.txt")));
    QCOMPARE(document_count(window), 0);
}

void StudentTests::remote_add_relative_path() {
    QTemporaryDir dir;
    write_file(dir, "a.txt");
    const QString socket = dir.filePath("s.sock");
    MainWindow window;
    QVERIFY(window.start_ipc_server(socket));

    QCOMPARE(run_send(socket, "a.txt", dir.path()).exit_code, 0);
    QTRY_COMPARE_WITH_TIMEOUT(document_count(window), 1, kWaitMs);
}

void StudentTests::remote_add_during_build_invalidates_it() {
    QTemporaryDir dir;
    const QString socket = dir.filePath("s.sock");
    MainWindow window;
    QVERIFY(window.start_ipc_server(socket));
    QVERIFY(window.add_document_file(write_file(dir, "a.txt")));
    window.set_build_delay_ms(60000);
    button(window, "Build Corpus")->click();

    QCOMPARE(run_send(socket, write_file(dir, "b.txt")).exit_code, 0);
    QTRY_COMPARE_WITH_TIMEOUT(document_count(window), 2, kWaitMs);
    QTRY_VERIFY_WITH_TIMEOUT(!window.build_in_progress(), kWaitMs);
    QVERIFY(!shows_chunks(window));
}

void StudentTests::remote_add_special_paths_data() {
    QTest::addColumn<QString>("name");
    QTest::newRow("space") << QString("with space.txt");
    QTest::newRow("utf8") << QString::fromUtf8("caf\xC3\xA9 \xE6\x96\x87.txt");
    QTest::newRow("newline") << QString("line\nbreak.txt");
}

void StudentTests::remote_add_special_paths() {
    QFETCH(QString, name);
    QTemporaryDir dir;
    const QString socket = dir.filePath("s.sock");
    const QString path = write_file(dir, name);
    QVERIFY(!path.isEmpty());
    MainWindow window;
    QVERIFY(window.start_ipc_server(socket));

    QCOMPARE(run_send(socket, path).exit_code, 0);
    QTRY_COMPARE_WITH_TIMEOUT(document_count(window), 1, kWaitMs);
    QCOMPARE(window.findChild<QListWidget*>()->item(0)->text(), name);
}

void StudentTests::unreadable_file_exits_1() {
    QTemporaryDir dir;
    const QString socket = dir.filePath("s.sock");
    MainWindow window;
    QVERIFY(window.start_ipc_server(socket));

    const SendResult result = run_send(socket, dir.filePath("missing.txt"));
    QCOMPARE(result.exit_code, 1);
    QVERIFY(!result.error.isEmpty());
    QTest::qWait(100);
    QCOMPARE(document_count(window), 0);
}

void StudentTests::no_application_exits_2() {
    QTemporaryDir dir;
    QElapsedTimer timer;
    timer.start();
    const SendResult result = run_send(dir.filePath("none.sock"), "a.txt", dir.path());
    QCOMPARE(result.exit_code, 2);
    QVERIFY(timer.elapsed() < 2500);
}

void StudentTests::request_split_into_pieces_is_received() {
    QTemporaryDir dir;
    const QString socket = dir.filePath("s.sock");
    const std::string path = write_file(dir, "a.txt").toStdString();
    MainWindow window;
    QVERIFY(window.start_ipc_server(socket));

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    QVERIFY(fd >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string socket_path = socket.toStdString();
    QVERIFY(socket_path.size() < sizeof(address.sun_path));
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    QCOMPARE(::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);

    std::string request;
    aiws::ipc::append_u32(request, static_cast<std::uint32_t>(path.size()));
    request += path;
    for (char byte : request) {
        QCOMPARE(::send(fd, &byte, 1, MSG_NOSIGNAL), ssize_t{1});
        QTest::qSleep(2);
    }

    unsigned char reply[5] = {};
    std::size_t received = 0;
    while (received < sizeof(reply)) {
        ssize_t n = ::recv(fd, reply + received, sizeof(reply) - received, 0);
        QVERIFY(n > 0);
        received += static_cast<std::size_t>(n);
    }
    ::close(fd);

    QCOMPARE(reply[0], aiws::ipc::kStatusAccepted);
    QTRY_COMPARE_WITH_TIMEOUT(document_count(window), 1, kWaitMs);
}

void StudentTests::socket_removed_within_500ms_of_exit() {
    QTemporaryDir dir;
    const QString socket = dir.filePath("s.sock");
    QElapsedTimer timer;
    {
        MainWindow window;
        QVERIFY(window.start_ipc_server(socket));
        QVERIFY(QFile::exists(socket));
        timer.start();
    }
    QVERIFY(timer.elapsed() < 500);
    QVERIFY(!QFile::exists(socket));
    QCOMPARE(run_send(socket, "a.txt", dir.path()).exit_code, 2);
}

QTEST_MAIN(StudentTests)
#include "student_tests.moc"
