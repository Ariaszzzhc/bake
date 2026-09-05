// bake.lsp — worker implementation: one request at a time.
//
// `bake lsp-worker` is the process the language server master
// spawns. It reads a framed request from stdin, runs exactly one
// frontend job through bake.toolchain.compilation (the same
// seams the in-process server used, unchanged), and writes the
// framed response to stdout. There is no concurrency here —
// parallelism comes from the master running several workers, so
// the process-wide cc1 mutex never contends inside one worker.

module;

#include <unistd.h>

module bake.lsp;
// TEMP-DEBUG

import std;
import bake.lsp.proto;
import bake.toolchain.compilation;

namespace bake {

namespace {

using lsp_proto::WireRequest;
using lsp_proto::WireResponse;
using lsp_proto::WireDiagnostic;
using lsp_proto::WireOccurrence;

// Blocking exact-size read; false on EOF or error.
bool read_exact(int fd, char* dst, std::size_t size) {
    std::size_t done = 0;
    while (done < size) {
        ssize_t n = ::read(fd, dst + done, size - done);
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

// Blocking exact-size write; false on error (EPIPE included).
bool write_exact(int fd, const char* src, std::size_t size) {
    std::size_t done = 0;
    while (done < size) {
        ssize_t n = ::write(fd, src + done, size - done);
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

std::optional<std::string> read_frame() {
    char header[4];
    if (!read_exact(0, header, 4)) return std::nullopt;
    std::uint32_t len =
        (static_cast<unsigned char>(header[0]) << 24) |
        (static_cast<unsigned char>(header[1]) << 16) |
        (static_cast<unsigned char>(header[2]) << 8) |
        static_cast<unsigned char>(header[3]);
    if (len > 512u * 1024 * 1024) return std::nullopt;
    std::string payload(len, '\0');
    if (len > 0 && !read_exact(0, payload.data(), len))
        return std::nullopt;
    return payload;
}

bool write_frame(std::string_view frame) {
    return write_exact(1, frame.data(), frame.size());
}

std::vector<WireDiagnostic> to_wire(
        const std::vector<SourceDiagnostic>& diags) {
    std::vector<WireDiagnostic> out;
    out.reserve(diags.size());
    for (const auto& d : diags) {
        WireDiagnostic w;
        w.severity = d.severity;
        w.file = d.file;
        w.line = d.line;
        w.column = d.column;
        w.message = d.message;
        out.push_back(std::move(w));
    }
    return out;
}

WireResponse run_one(const WireRequest& req) {
    WireResponse resp;
    resp.id = req.id;

    if (req.op == "syntax") {
        CompileOptions options;
        if (!req.pch.empty())
            options.pch = PchLoad{req.pch.path, req.pch.size,
                                  req.pch.ends_at_start_of_line,
                                  req.pch.region_limit};
        CompileOutcome outcome =
            run_syntax_check(req.argv, req.remap, options);
        resp.ok = true;
        resp.status = outcome.status;
        resp.diagnostics = to_wire(outcome.diagnostics);
    } else if (req.op == "preamble") {
        PreambleBuild build = run_preamble_build(
            req.argv, req.remap, req.main_file, req.pch_out,
            req.region_limit);
        resp.ok = true;
        resp.status = build.outcome.status;
        resp.built = build.ok;
        resp.preamble_size = build.preamble_size;
        resp.ends_at_start_of_line = build.preamble_ends_at_start_of_line;
        resp.diagnostics = to_wire(build.outcome.diagnostics);
    } else if (req.op == "complete") {
        auto items = run_code_complete(
            req.argv, req.remap, req.main_file, req.line, req.column);
        resp.ok = true;
        for (auto& item : items) {
            lsp_proto::WireCompletion w;
            w.label = std::move(item.label);
            w.detail = std::move(item.detail);
            w.kind = std::move(item.kind);
            w.priority = item.priority;
            resp.items.push_back(std::move(w));
        }
    } else if (req.op == "collect") {
        auto occurrences =
            run_symbol_collection(req.argv, req.remap, req.main_file);
        resp.ok = true;
        for (auto& occ : occurrences) {
            unsigned flags = 0;
            if (occ.is_decl) flags |= 1;
            if (occ.is_def) flags |= 2;
            if (occ.is_ref) flags |= 4;
            if (occ.is_call) flags |= 8;
            WireOccurrence w;
            w.usr = std::move(occ.usr);
            w.name = std::move(occ.name);
            w.kind = std::move(occ.kind);
            w.flags = flags;
            w.file = std::move(occ.file);
            w.line = occ.line;
            w.column = occ.column;
            w.decl_file = std::move(occ.decl_file);
            w.decl_line = occ.decl_line;
            w.decl_column = occ.decl_column;
            resp.occurrences.push_back(std::move(w));
        }
    } else {
        resp.ok = false;
        resp.error = "unknown op '" + req.op + "'";
    }
    return resp;
}

}  // namespace

int lsp_worker_main([[maybe_unused]] const std::vector<std::string>& args) {
    for (;;) {
        auto frame = read_frame();
        if (!frame) return 0;  // master closed the pipe: clean exit
        auto req = lsp_proto::decode_request(*frame);
        if (!req) {
            // A malformed stream cannot be resynchronized; answer
            // with an error and quit so the master respawns clean.
            WireResponse resp;
            resp.ok = false;
            resp.error = req.error();
            write_frame(lsp_proto::encode_response(resp));
            return 1;
        }
        if (!write_frame(lsp_proto::encode_response(run_one(*req))))
            return 1;  // master went away mid-reply
    }
}

}  // namespace bake
