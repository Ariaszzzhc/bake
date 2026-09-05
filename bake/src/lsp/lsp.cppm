export module bake.lsp;

import std;

// ============================================================
// bake.lsp — LSP server entry (stage-split interface)
//
// The language server itself is stage1-only: it rides on the
// kotatsu async runtime and the bake.toolchain.compilation /
// bake.buildsystem.query seams, none of which the CMake
// bootstrap carries. Stage0 provides a stub implementation that
// explains this; the self-hosted build compiles the real server
// (src/lsp/lsp_core.cpp).
// ============================================================

namespace bake {

// Entry point for `bake lsp`. `args` are the raw argv after the
// subcommand.
export int lsp_main(const std::vector<std::string>& args);

// Entry point for `bake lsp-worker`, the process the master
// spawns for every frontend job. Same argv shape as lsp_main.
export int lsp_worker_main(const std::vector<std::string>& args);

} // namespace bake
