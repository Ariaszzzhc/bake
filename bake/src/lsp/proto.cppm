export module bake.lsp.proto;

import std;
import bake.json;

// ============================================================
// bake.lsp.proto — master ↔ worker wire protocol
//
// The language server master owns the editor-facing protocol and
// the symbol index; every Clang frontend run happens in a worker
// process (`bake lsp-worker`). Both sides speak this protocol:
// length-prefixed JSON frames over the worker's stdin/stdout
// pipes — 4-byte big-endian payload length, then the payload.
//
// One worker serves one request at a time. The master pairs
// responses to requests by id; there is no worker-side
// concurrency and therefore no interleaving.
//
// The payload types are deliberately self-contained (no
// bake.toolchain imports): the codec stays testable and the
// master never needs the compilation module to talk protocol.
// ============================================================

namespace bake::lsp_proto {

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

export struct WireOccurrence {
    std::string usr;
    std::string name;
    std::string kind;
    unsigned flags = 0;  // 1 decl | 2 def | 4 ref | 8 call
    std::string file;
    unsigned line = 1;
    unsigned column = 1;
    std::string decl_file;
    unsigned decl_line = 0;
    unsigned decl_column = 0;
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
    std::vector<WireOccurrence> occurrences;
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

// Extract one complete frame from the front of `buffer` (data
// accumulated so far). Returns nullopt while more bytes are
// needed; on success the frame's payload is removed from the
// buffer and returned. A length prefix that cannot ever arrive
// (larger than any legal message) reports an error string.
export std::expected<std::string, std::string> take_frame(
        std::string& buffer) {
    if (buffer.size() < 4) return std::unexpected("need more data");
    std::uint32_t len = (static_cast<unsigned char>(buffer[0]) << 24) |
                        (static_cast<unsigned char>(buffer[1]) << 16) |
                        (static_cast<unsigned char>(buffer[2]) << 8) |
                        static_cast<unsigned char>(buffer[3]);
    // Responses for the largest TUs stay well under this; a bogus
    // prefix means the stream desynchronized and must be torn down.
    if (len > 512u * 1024 * 1024)
        return std::unexpected("frame length prefix out of range");
    if (buffer.size() - 4 < len) return std::unexpected("need more data");
    std::string payload = buffer.substr(4, len);
    buffer.erase(0, 4 + static_cast<std::size_t>(len));
    return payload;
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

json::Value to_json(const WireOccurrence& o) {
    json::Value v = json::Value::object();
    v["usr"] = o.usr;
    v["name"] = o.name;
    v["kind"] = o.kind;
    v["flags"] = std::int64_t(o.flags);
    v["file"] = o.file;
    v["line"] = std::int64_t(o.line);
    v["col"] = std::int64_t(o.column);
    v["dfile"] = o.decl_file;
    v["dline"] = std::int64_t(o.decl_line);
    v["dcol"] = std::int64_t(o.decl_column);
    return v;
}

WireOccurrence occurrence_from_json(const json::Value& v) {
    WireOccurrence o;
    if (const json::Value* f = v.find("usr"))
        o.usr = f->get<std::string>();
    if (const json::Value* f = v.find("name"))
        o.name = f->get<std::string>();
    if (const json::Value* f = v.find("kind"))
        o.kind = f->get<std::string>();
    if (const json::Value* f = v.find("flags"))
        o.flags = static_cast<unsigned>(f->get<std::int64_t>());
    if (const json::Value* f = v.find("file"))
        o.file = f->get<std::string>();
    if (const json::Value* f = v.find("line"))
        o.line = static_cast<unsigned>(f->get<std::int64_t>());
    if (const json::Value* f = v.find("col"))
        o.column = static_cast<unsigned>(f->get<std::int64_t>());
    if (const json::Value* f = v.find("dfile"))
        o.decl_file = f->get<std::string>();
    if (const json::Value* f = v.find("dline"))
        o.decl_line = static_cast<unsigned>(f->get<std::int64_t>());
    if (const json::Value* f = v.find("dcol"))
        o.decl_column = static_cast<unsigned>(f->get<std::int64_t>());
    return o;
}

}  // namespace

export std::string encode_request(const WireRequest& req) {
    json::Value root = json::Value::object();
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
    return encode_frame(root.dump());
}

export std::expected<WireRequest, std::string> decode_request(
        std::string_view payload) {
    try {
        json::Value root = json::Value::parse(payload);
        WireRequest req;
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
        json::Value arr = json::Value::array();
        for (const auto& o : resp.occurrences)
            arr.push_back(to_json(o));
        root["occs"] = std::move(arr);
    }
    return encode_frame(root.dump());
}

export std::expected<WireResponse, std::string> decode_response(
        std::string_view payload) {
    try {
        json::Value root = json::Value::parse(payload);
        WireResponse resp;
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
            for (std::size_t i = 0; i < f->size(); ++i)
                resp.occurrences.push_back(occurrence_from_json((*f)[i]));
        }
        return resp;
    } catch (const json::Error& e) {
        return std::unexpected(std::string("bad response json: ") +
                               e.what());
    }
}

}  // namespace bake::lsp_proto
