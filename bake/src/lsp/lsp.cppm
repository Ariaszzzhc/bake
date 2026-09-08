export module bake.lsp;

import std;

// ============================================================
// bake.lsp — LSP server entry (stage-split interface)
//
// The language server itself is stage1-only. Stage0 provides the
// bootstrap command surface, while Stage1 adapts LSP messages to Core.
// ============================================================

namespace bake {

// Entry point for `bake lsp`. `args` are the raw argv after the
// subcommand.
export int lsp_main(const std::vector<std::string>& args);

// Entry point for `bake lsp-worker`, the process the master
// spawns for every frontend job. Same argv shape as lsp_main.
export int lsp_worker_main(const std::vector<std::string>& args);

} // namespace bake
