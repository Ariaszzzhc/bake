// bake.lsp — stage0 stub. The language server needs the kotatsu
// async runtime (a stage1 bake-pkgs dependency) and is therefore
// only present in the self-hosted binary.

module bake.lsp;

import std;

namespace bake {

int lsp_main(const std::vector<std::string>& args) {
    std::println(std::cerr,
                 "bake: 'lsp' is not available in the bootstrap "
                 "(stage0) binary");
    std::println(std::cerr,
                 "Build the self-hosted binary first: bake build");
    return 1;
}

} // namespace bake
