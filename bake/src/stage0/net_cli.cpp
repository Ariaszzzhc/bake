// stage0 implementation — spawns the system curl binary. Compiled only
// by the CMake bootstrap; the self-hosted bake links the in-process
// libcurl transport instead (src/net.cpp).

module bake.net;

import std;
import bake.util;

namespace bake::net {

bool download(const std::string& url, const Path& dest) {
    auto result = run_process({"curl", "-sL", "--fail", "-o",
                               dest.string(), url},
                              Path(), true);
    return result.success();
}

}  // namespace bake::net
