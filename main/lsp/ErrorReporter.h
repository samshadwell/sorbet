#ifndef RUBY_TYPER_LSP_ERRORREPORTER_H
#define RUBY_TYPER_LSP_ERRORREPORTER_H

#include "core/core.h"

namespace sorbet::realmain::lsp {
class LSPConfiguration;
struct ErrorStatus {
    // The epoch at which we last sent diagnostics for this file.
    u4 lastReportedEpoch = 0;
    // If true, the client believes this file has errors.
    bool hasErrors = false;
};
class ErrorReporter {
    const std::shared_ptr<const LSPConfiguration> config;
    // Maps from file ref ID to its error status.
    std::vector<ErrorStatus> fileErrorStatuses;
    ErrorStatus &getFileErrorStatus(core::FileRef file);

public:
    ErrorReporter(std::shared_ptr<const LSPConfiguration> config);
    std::vector<core::FileRef> filesWithErrorsSince(u4 epoch);
    /**
     * Sends diagnostics from a typecheck run of a single file to the client.
     * `epoch` specifies the epoch of the file updates that produced these diagnostics. Used to prevent emitting
     * outdated diagnostics from a slow path run if they had already been re-typechecked on the fast path.
     */
    void pushDiagnostics(u4 epoch, core::FileRef file, const std::vector<std::unique_ptr<core::Error>> &errors,
                         const core::GlobalState &gs);
};
}; // namespace sorbet::realmain::lsp

#endif // RUBY_TYPER_LSP_ERRORREPORTER_H