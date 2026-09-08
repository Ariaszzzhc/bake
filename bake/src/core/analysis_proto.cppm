export module bake.core.analysis.proto;

import std;
import bake.core.occurrences;
import bake.json;
import bake.toolchain.compilation;

// ============================================================
// bake.core.analysis.proto — Core frontend worker protocol
//
// The language server owns the editor-facing protocol. Core owns
// the symbol index and worker runtime; every Clang frontend run
// happens in a worker process (`bake lsp-worker`). Both sides
// speak this protocol:
// length-prefixed JSON frames over the worker's stdin/stdout
// pipes — 4-byte big-endian payload length, then the payload.
//
// One worker serves one request at a time. The master pairs
// responses to requests by id; there is no worker-side
// concurrency and therefore no interleaving.
//
// ============================================================

namespace bake::analysis_proto {

// ── wire types ──────────────────────────────────────────────

export struct WireDiagnostic {
    std::string severity;  // "fatal"|"error"|"warning"|"note"|"remark"
    std::string file;
    unsigned line = 1;
    unsigned column = 1;
    std::string message;
};

export struct WireCompletion {
    std::string label;
    std::string detail;
    std::string kind;
    int priority = 0;
};

export struct WirePchLoad {
    std::string path;
    unsigned size = 0;
    bool ends_at_start_of_line = false;
    std::size_t region_limit = std::size_t(-1);  // truncation cap

    bool empty() const { return path.empty(); }
};

export struct WireRequest {
    std::uint64_t id = 0;
    std::string op;  // "syntax" | "preamble" | "complete" | "collect"
    std::vector<std::string> argv;
    std::map<std::string, std::string> remap;
    std::string main_file;
    // "preamble"
    std::string pch_out;
    std::size_t region_limit = std::size_t(-1);
    // "complete"
    unsigned line = 0;
    unsigned column = 0;
    // "syntax"
    WirePchLoad pch;
};

export struct WireResponse {
    std::uint64_t id = 0;
    bool ok = false;
    std::string error;
    int status = 0;
    // "preamble"
    bool built = false;
    unsigned preamble_size = 0;
    bool ends_at_start_of_line = false;
    // results
    std::vector<WireDiagnostic> diagnostics;
    std::vector<WireCompletion> items;
    std::vector<SymbolOccurrence> occurrences;
};

// ── framing ─────────────────────────────────────────────────

// A complete frame: the 4-byte big-endian length prefix plus the
// payload, ready to hand to a pipe write.
export std::string encode_frame(std::string_view payload) {
    std::uint32_t len = static_cast<std::uint32_t>(payload.size());
    std::string frame(4, '\0');
    frame[0] = static_cast<char>((len >> 24) & 0xff);
    frame[1] = static_cast<char>((len >> 16) & 0xff);
    frame[2] = static_cast<char>((len >> 8) & 0xff);
    frame[3] = static_cast<char>(len & 0xff);
    frame.append(payload);
    return frame;
}

// ── codec ───────────────────────────────────────────────────

namespace {

json::Value to_json(const WireDiagnostic& d) {
    json::Value v = json::Value::object();
    v["sev"] = d.severity;
    v["file"] = d.file;
    v["line"] = std::int64_t(d.line);
    v["col"] = std::int64_t(d.column);
    v["msg"] = d.message;
    return v;
}

WireDiagnostic diagnostic_from_json(const json::Value& v) {
    WireDiagnostic d;
    if (const json::Value* f = v.find("sev"))
        d.severity = f->get<std::string>();
    if (const json::Value* f = v.find("file"))
        d.file = f->get<std::string>();
    if (const json::Value* f = v.find("line"))
        d.line = static_cast<unsigned>(f->get<std::int64_t>());
    if (const json::Value* f = v.find("col"))
        d.column = static_cast<unsigned>(f->get<std::int64_t>());
    if (const json::Value* f = v.find("msg"))
        d.message = f->get<std::string>();
    return d;
}

json::Value to_json(const WireCompletion& c) {
    json::Value v = json::Value::object();
    v["label"] = c.label;
    v["detail"] = c.detail;
    v["kind"] = c.kind;
    v["prio"] = std::int64_t(c.priority);
    return v;
}

WireCompletion completion_from_json(const json::Value& v) {
    WireCompletion c;
    if (const json::Value* f = v.find("label"))
        c.label = f->get<std::string>();
    if (const json::Value* f = v.find("detail"))
        c.detail = f->get<std::string>();
    if (const json::Value* f = v.find("kind"))
        c.kind = f->get<std::string>();
    if (const json::Value* f = v.find("prio"))
        c.priority = static_cast<int>(f->get<std::int64_t>());
    return c;
}

}  // namespace

export std::string encode_request(const WireRequest& req) {
    json::Value root = json::Value::object();
    root["schema"] = "bake-analysis-wire-v2";
    root["id"] = std::int64_t(req.id);
    root["op"] = req.op;
    json::Value argv = json::Value::array();
    for (const auto& a : req.argv) argv.push_back(a);
    root["argv"] = std::move(argv);
    json::Value remap = json::Value::object();
    for (const auto& [k, val] : req.remap) remap[k] = val;
    root["remap"] = std::move(remap);
    if (!req.main_file.empty()) root["main"] = req.main_file;
    if (!req.pch_out.empty()) {
        root["pch_out"] = req.pch_out;
        root["limit"] = std::int64_t(req.region_limit);
    }
    if (req.line != 0) {
        root["line"] = std::int64_t(req.line);
        root["col"] = std::int64_t(req.column);
    }
    if (!req.pch.empty()) {
        json::Value pch = json::Value::object();
        pch["path"] = req.pch.path;
        pch["size"] = std::int64_t(req.pch.size);
        pch["sol"] = req.pch.ends_at_start_of_line;
        pch["limit"] = std::int64_t(req.pch.region_limit);
        root["pch"] = std::move(pch);
    }
    return root.dump();
}

export std::expected<WireRequest, std::string> decode_request(
        std::string_view payload) {
    try {
        json::Value root = json::Value::parse(payload);
        WireRequest req;
        const json::Value* schema = root.find("schema");
        if (!schema || !schema->is_string() ||
            schema->get<std::string>() != "bake-analysis-wire-v2")
            return std::unexpected("request wire identity mismatch");
        if (const json::Value* f = root.find("id"))
            req.id = static_cast<std::uint64_t>(f->get<std::int64_t>());
        if (const json::Value* f = root.find("op"))
            req.op = f->get<std::string>();
        else return std::unexpected("request without op");
        if (const json::Value* f = root.find("argv")) {
            for (std::size_t i = 0; i < f->size(); ++i)
                req.argv.push_back((*f)[i].get<std::string>());
        }
        if (const json::Value* f = root.find("remap")) {
            for (const auto& member : f->items())
                req.remap[member.key()] =
                    member.value().get<std::string>();
        }
        if (const json::Value* f = root.find("main"))
            req.main_file = f->get<std::string>();
        if (const json::Value* f = root.find("pch_out")) {
            req.pch_out = f->get<std::string>();
            req.region_limit = std::size_t(-1);
            if (const json::Value* l = root.find("limit"))
                req.region_limit =
                    static_cast<std::size_t>(l->get<std::int64_t>());
        }
        if (const json::Value* f = root.find("line")) {
            req.line = static_cast<unsigned>(f->get<std::int64_t>());
            if (const json::Value* c = root.find("col"))
                req.column = static_cast<unsigned>(c->get<std::int64_t>());
        }
        if (const json::Value* f = root.find("pch")) {
            req.pch.path = f->find("path")->get<std::string>();
            req.pch.size = static_cast<unsigned>(
                f->find("size")->get<std::int64_t>());
            req.pch.ends_at_start_of_line = false;
            if (const json::Value* s = f->find("sol"))
                req.pch.ends_at_start_of_line = s->get<bool>();
            req.pch.region_limit = std::size_t(-1);
            if (const json::Value* l = f->find("limit"))
                req.pch.region_limit =
                    static_cast<std::size_t>(l->get<std::int64_t>());
        }
        return req;
    } catch (const json::Error& e) {
        return std::unexpected(std::string("bad request json: ") +
                               e.what());
    }
}

export std::string encode_response(const WireResponse& resp) {
    json::Value root = json::Value::object();
    root["schema"] = "bake-analysis-wire-v2";
    root["id"] = std::int64_t(resp.id);
    root["ok"] = resp.ok;
    if (!resp.ok) {
        root["error"] = resp.error;
        return encode_frame(root.dump());
    }
    root["status"] = std::int64_t(resp.status);
    if (resp.built || resp.preamble_size != 0) {
        root["built"] = resp.built;
        root["size"] = std::int64_t(resp.preamble_size);
        root["sol"] = resp.ends_at_start_of_line;
    }
    if (!resp.diagnostics.empty()) {
        json::Value arr = json::Value::array();
        for (const auto& d : resp.diagnostics) arr.push_back(to_json(d));
        root["diags"] = std::move(arr);
    }
    if (!resp.items.empty()) {
        json::Value arr = json::Value::array();
        for (const auto& c : resp.items) arr.push_back(to_json(c));
        root["items"] = std::move(arr);
    }
    if (!resp.occurrences.empty()) {
        auto batch = encode_occurrence_batch(resp.occurrences);
        if (!batch) {
            root["ok"] = false;
            root["error"] = batch.error().message;
            return encode_frame(root.dump());
        }
        root["occs"] = std::move(*batch);
    }
    return encode_frame(root.dump());
}

export std::expected<WireResponse, std::string> decode_response(
        std::string_view payload) {
    try {
        json::Value root = json::Value::parse(payload);
        WireResponse resp;
        const json::Value* schema = root.find("schema");
        if (!schema || !schema->is_string() ||
            schema->get<std::string>() != "bake-analysis-wire-v2")
            return std::unexpected("response wire identity mismatch");
        if (const json::Value* f = root.find("id"))
            resp.id = static_cast<std::uint64_t>(f->get<std::int64_t>());
        if (const json::Value* f = root.find("ok"))
            resp.ok = f->get<bool>();
        else return std::unexpected("response without ok");
        if (!resp.ok) {
            if (const json::Value* f = root.find("error"))
                resp.error = f->get<std::string>();
            return resp;
        }
        if (const json::Value* f = root.find("status"))
            resp.status = static_cast<int>(f->get<std::int64_t>());
        if (const json::Value* f = root.find("built")) {
            resp.built = f->get<bool>();
            if (const json::Value* s = root.find("size"))
                resp.preamble_size =
                    static_cast<unsigned>(s->get<std::int64_t>());
            if (const json::Value* s = root.find("sol"))
                resp.ends_at_start_of_line = s->get<bool>();
        }
        if (const json::Value* f = root.find("diags")) {
            for (std::size_t i = 0; i < f->size(); ++i)
                resp.diagnostics.push_back(
                    diagnostic_from_json((*f)[i]));
        }
        if (const json::Value* f = root.find("items")) {
            for (std::size_t i = 0; i < f->size(); ++i)
                resp.items.push_back(completion_from_json((*f)[i]));
        }
        if (const json::Value* f = root.find("occs")) {
            auto occurrences = decode_occurrence_batch(*f);
            if (!occurrences)
                return std::unexpected(occurrences.error().message);
            resp.occurrences = std::move(*occurrences);
        }
        return resp;
    } catch (const json::Error& e) {
        return std::unexpected(std::string("bad response json: ") +
                               e.what());
    }
}

}  // namespace bake::analysis_proto
