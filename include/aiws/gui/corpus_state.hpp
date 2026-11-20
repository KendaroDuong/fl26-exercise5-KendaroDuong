#pragma once

#include "aiws/processing_core.hpp"

#include <QString>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace aiws::gui {

// Operations available to states; implemented by MainWindow on the GUI thread.
class CorpusContext {
public:
    virtual ~CorpusContext() = default;

    /** @brief Hands a snapshot of the workspace to the worker. Returns the request id. */
    virtual std::uint64_t start_build() = 0;
    /** @brief Makes the running or pending build unwanted. Does not wait. */
    virtual void cancel_build() = 0;
    /** @brief Makes core the corpus that Search, Inspect and Context use. */
    virtual void install_corpus(ProcessingCore core) = 0;
    virtual std::size_t corpus_chunk_count() const = 0;
    /** @brief Sets the "Corpus: ..." label. */
    virtual void show_corpus_status(const QString& text) = 0;
    /** @brief Sets the status bar message. */
    virtual void show_message(const QString& text) = 0;
    /** @brief Clears search results, context and term statistics. */
    virtual void clear_processing_outputs() = 0;
};

// Events return a new state, or nullptr to keep the current state.
class CorpusState {
public:
    virtual ~CorpusState() = default;

    /** @brief Called once, right after this state becomes current. */
    virtual void enter(CorpusContext& context) = 0;

    /** @brief For display and debugging only; never decides a transition. */
    virtual QString name() const = 0;

    /** @brief True only if the corpus reflects the present workspace. */
    virtual bool corpus_is_current() const { return false; }
    /** @brief True while a build for the present workspace is wanted. */
    virtual bool build_running() const { return false; }
    /** @brief Whether the Build Corpus button may be enabled (with documents). */
    virtual bool accepts_build_request() const { return true; }

    // ---- Events ---------------------------------------------------------
    virtual std::unique_ptr<CorpusState> build_requested(CorpusContext& context);
    virtual std::unique_ptr<CorpusState> workspace_changed(CorpusContext& context);
    virtual std::unique_ptr<CorpusState> reset(CorpusContext& context);
    virtual std::unique_ptr<CorpusState> build_succeeded(
        CorpusContext& context, std::uint64_t id, ProcessingCore& core);
    virtual std::unique_ptr<CorpusState> build_failed(
        CorpusContext& context, std::uint64_t id, const QString& reason);
};

/** @brief No corpus, and nothing built since the last Reset (initial state). */
class NotBuiltState final : public CorpusState {
public:
    void enter(CorpusContext& context) override;
    QString name() const override { return QStringLiteral("NotBuilt"); }
    std::unique_ptr<CorpusState> workspace_changed(CorpusContext& context) override;
};

/** @brief A build for the present workspace is pending or running on the worker. */
class BuildingState final : public CorpusState {
public:
    explicit BuildingState(std::uint64_t build_id) : build_id_(build_id) {}

    void enter(CorpusContext& context) override;
    QString name() const override { return QStringLiteral("Building"); }
    bool build_running() const override { return true; }
    bool accepts_build_request() const override { return false; }

    std::unique_ptr<CorpusState> build_requested(CorpusContext& context) override;
    std::unique_ptr<CorpusState> workspace_changed(CorpusContext& context) override;
    std::unique_ptr<CorpusState> reset(CorpusContext& context) override;
    std::unique_ptr<CorpusState> build_succeeded(
        CorpusContext& context, std::uint64_t id, ProcessingCore& core) override;
    std::unique_ptr<CorpusState> build_failed(
        CorpusContext& context, std::uint64_t id, const QString& reason) override;

private:
    std::uint64_t build_id_;  // The only build whose result this state accepts.
};

/** @brief The corpus reflects the present workspace; Search, Inspect, Context work. */
class CurrentState final : public CorpusState {
public:
    void enter(CorpusContext& context) override;
    QString name() const override { return QStringLiteral("Current"); }
    bool corpus_is_current() const override { return true; }
};

/** @brief The workspace changed after a build was made or started. */
class OutOfDateState final : public CorpusState {
public:
    void enter(CorpusContext& context) override;
    QString name() const override { return QStringLiteral("OutOfDate"); }
    std::unique_ptr<CorpusState> workspace_changed(CorpusContext& context) override;
};

/** @brief The last build for the present workspace threw. */
class FailedState final : public CorpusState {
public:
    explicit FailedState(QString reason) : reason_(std::move(reason)) {}

    void enter(CorpusContext& context) override;
    QString name() const override { return QStringLiteral("Failed"); }

private:
    QString reason_;
};

}  // namespace aiws::gui
