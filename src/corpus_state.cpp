#include "aiws/gui/corpus_state.hpp"

#include <utility>

namespace aiws::gui {

std::unique_ptr<CorpusState> CorpusState::build_requested(CorpusContext& context) {
    return std::make_unique<BuildingState>(context.start_build());
}

std::unique_ptr<CorpusState> CorpusState::workspace_changed(CorpusContext&) {
    return std::make_unique<OutOfDateState>();
}

std::unique_ptr<CorpusState> CorpusState::reset(CorpusContext&) {
    return std::make_unique<NotBuiltState>();
}

std::unique_ptr<CorpusState> CorpusState::build_succeeded(
    CorpusContext&, std::uint64_t, ProcessingCore&) {
    return nullptr;
}

std::unique_ptr<CorpusState> CorpusState::build_failed(
    CorpusContext&, std::uint64_t, const QString&) {
    return nullptr;
}

void NotBuiltState::enter(CorpusContext& context) {
    context.show_corpus_status("Corpus: Not built");
}

std::unique_ptr<CorpusState> NotBuiltState::workspace_changed(CorpusContext&) {
    return nullptr;
}

void BuildingState::enter(CorpusContext& context) {
    context.clear_processing_outputs();
    context.show_corpus_status("Corpus: Building...");
    context.show_message("Building corpus...");
}

std::unique_ptr<CorpusState> BuildingState::build_requested(CorpusContext&) {
    return nullptr;
}

std::unique_ptr<CorpusState> BuildingState::workspace_changed(CorpusContext& context) {
    context.cancel_build();
    return std::make_unique<OutOfDateState>();
}

std::unique_ptr<CorpusState> BuildingState::reset(CorpusContext& context) {
    context.cancel_build();
    return std::make_unique<NotBuiltState>();
}

std::unique_ptr<CorpusState> BuildingState::build_succeeded(
    CorpusContext& context, std::uint64_t id, ProcessingCore& core) {
    if (id != build_id_) {
        return nullptr;
    }
    context.install_corpus(std::move(core));
    return std::make_unique<CurrentState>();
}

std::unique_ptr<CorpusState> BuildingState::build_failed(
    CorpusContext&, std::uint64_t id, const QString& reason) {
    if (id != build_id_) {
        return nullptr;
    }
    return std::make_unique<FailedState>(reason);
}

void CurrentState::enter(CorpusContext& context) {
    context.show_corpus_status(
        QString("Corpus: %1 chunks").arg(static_cast<qulonglong>(context.corpus_chunk_count())));
}

void OutOfDateState::enter(CorpusContext& context) {
    context.clear_processing_outputs();
    context.show_corpus_status("Corpus: Out of date (Build Corpus required)");
}

std::unique_ptr<CorpusState> OutOfDateState::workspace_changed(CorpusContext&) {
    return nullptr;
}

void FailedState::enter(CorpusContext& context) {
    context.show_corpus_status("Corpus: Build failed");
    context.show_message(QString("Build Corpus failed: %1").arg(reason_));
}

}  // namespace aiws::gui
