// stage1 implementation — in-process libcurl (bake-pkgs "curl"
// package, mbedTLS backend with per-platform CA bundle paths baked in).
// Replaces the stage0 system-binary transport (src/stage0/net_cli.cpp).

module;

#include <curl/curl.h>

module bake.net;

import std;
import bake.util;

namespace bake::net {

namespace {

// libcurl requires exactly one curl_global_init across the process.
bool global_init() {
    static bool ok = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    return ok;
}

std::size_t write_cb(char* ptr, std::size_t size, std::size_t nmemb,
                      void* userdata) {
    std::FILE* f = static_cast<std::FILE*>(userdata);
    return std::fwrite(ptr, size, nmemb, f);
}

}  // namespace

bool download(const std::string& url, const Path& dest) {
    if (!global_init()) return false;

    CURL* handle = curl_easy_init();
    if (!handle) return false;

    Path tmp = dest.parent() /
               ("." + dest.filename_string() + ".part");
    std::FILE* f = std::fopen(tmp.native().c_str(), "wb");
    if (!f) {
        curl_easy_cleanup(handle);
        return false;
    }

    // Parity with the stage0 transport (`curl -sL --fail -o`).
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, f);

    CURLcode rc = curl_easy_perform(handle);
    curl_easy_cleanup(handle);
    if (std::fclose(f) != 0) rc = CURLE_WRITE_ERROR;

    if (rc != CURLE_OK) {
        tmp.remove();
        return false;
    }

    std::error_code error;
    std::filesystem::rename(tmp.fs(), dest.fs(), error);
    if (error) {
        tmp.remove();
        return false;
    }
    return true;
}

}  // namespace bake::net
