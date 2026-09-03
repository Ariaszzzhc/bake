export module bake.net;

import std;
import bake.util;

// ============================================================
// bake.net — HTTP(S) download for the package fetcher.
//
// The interface owns the contract; the transport is a
// stage-selected implementation unit of this same module:
//
//   stage0/net_cli.cpp   spawns the system curl binary (CMake bootstrap)
//   net.cpp              in-process libcurl (bake-pkgs "curl" package)
// ============================================================

export namespace bake::net {

// Download `url` over HTTP(S), following redirects, failing on any
// non-2xx response after redirects, writing the body to `dest`.
// `dest`'s parent directory must exist. Proxy environment variables
// (http_proxy/https_proxy/no_proxy) and CURL_CA_BUNDLE are honored by
// the libcurl implementation. Returns false on any failure; `dest` may
// hold a partial body afterwards — callers treat it as scratch.
bool download(const std::string& url, const Path& dest);

}  // namespace bake::net
