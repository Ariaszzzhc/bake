export module bake.core.occurrences;

import std;
import bake.json;
import bake.toolchain.compilation;

namespace bake {

export struct OccurrenceCodecError { std::string message; };

namespace occurrence_detail {

constexpr std::size_t row_width = 10;

unsigned flags(const SymbolOccurrence& occurrence) {
    unsigned result = 0;
    if (occurrence.is_decl) result |= 1;
    if (occurrence.is_def) result |= 2;
    if (occurrence.is_ref) result |= 4;
    if (occurrence.is_call) result |= 8;
    return result;
}

std::expected<std::int64_t, OccurrenceCodecError> integer(
        const json::Value& value, std::string_view field) {
    if (!value.is_integer())
        return std::unexpected(OccurrenceCodecError{
            "occurrence " + std::string(field) + " must be an integer"});
    return value.get<std::int64_t>();
}

std::expected<std::string, OccurrenceCodecError> string_at(
        const json::Value& strings, const json::Value& value,
        std::string_view field) {
    auto index = integer(value, field);
    if (!index) return std::unexpected(index.error());
    if (*index < 0 || static_cast<std::uint64_t>(*index) >= strings.size())
        return std::unexpected(OccurrenceCodecError{
            "occurrence " + std::string(field) + " index is out of range"});
    return strings[static_cast<std::size_t>(*index)].get<std::string>();
}

}

export std::expected<json::Value, OccurrenceCodecError>
encode_occurrence_batch(const std::vector<SymbolOccurrence>& occurrences) {
    for (const auto& occurrence : occurrences)
        if (occurrence.line < 1 || occurrence.column < 1)
            return std::unexpected(OccurrenceCodecError{
                "occurrence source position is out of range"});

    std::set<std::string> unique;
    for (const auto& occurrence : occurrences) {
        unique.insert(occurrence.usr);
        unique.insert(occurrence.name);
        unique.insert(occurrence.kind);
        unique.insert(occurrence.file);
        unique.insert(occurrence.decl_file);
    }
    std::vector<std::string> strings(unique.begin(), unique.end());
    std::map<std::string, std::int64_t> indices;
    for (std::size_t i = 0; i < strings.size(); ++i)
        indices.emplace(strings[i], static_cast<std::int64_t>(i));

    json::Value encoded_strings = json::Value::array();
    for (const auto& value : strings) encoded_strings.push_back(value);
    json::Value rows = json::Value::array();
    for (const auto& occurrence : occurrences) {
        json::Value row = json::Value::array();
        row.push_back(indices.at(occurrence.usr));
        row.push_back(indices.at(occurrence.name));
        row.push_back(indices.at(occurrence.kind));
        row.push_back(std::int64_t(occurrence_detail::flags(occurrence)));
        row.push_back(indices.at(occurrence.file));
        row.push_back(std::int64_t(occurrence.line));
        row.push_back(std::int64_t(occurrence.column));
        row.push_back(indices.at(occurrence.decl_file));
        row.push_back(std::int64_t(occurrence.decl_line));
        row.push_back(std::int64_t(occurrence.decl_column));
        rows.push_back(std::move(row));
    }
    json::Value result = json::Value::object();
    result["strings"] = std::move(encoded_strings);
    result["rows"] = std::move(rows);
    return result;
}

export std::expected<std::vector<SymbolOccurrence>, OccurrenceCodecError>
decode_occurrence_batch(const json::Value& batch) {
    if (!batch.is_object() || batch.size() != 2)
        return std::unexpected(OccurrenceCodecError{
            "occurrence batch has unexpected fields"});
    const json::Value* strings = batch.find("strings");
    const json::Value* rows = batch.find("rows");
    if (!strings || !strings->is_array())
        return std::unexpected(OccurrenceCodecError{
            "occurrence string table must be an array"});
    if (!rows || !rows->is_array())
        return std::unexpected(OccurrenceCodecError{
            "occurrence rows must be an array"});
    for (std::size_t i = 0; i < strings->size(); ++i) {
        if (!(*strings)[i].is_string())
            return std::unexpected(OccurrenceCodecError{
                "occurrence string table contains a non-string"});
        if (i > 0 && !((*strings)[i - 1].get<std::string>() <
                       (*strings)[i].get<std::string>()))
            return std::unexpected(OccurrenceCodecError{
                "occurrence string table must be strictly sorted"});
    }

    std::vector<SymbolOccurrence> result;
    result.reserve(rows->size());
    for (std::size_t i = 0; i < rows->size(); ++i) {
        const auto& row = (*rows)[i];
        if (!row.is_array() || row.size() != occurrence_detail::row_width)
            return std::unexpected(OccurrenceCodecError{
                "occurrence row must contain exactly ten fields"});
        auto usr = occurrence_detail::string_at(*strings, row[0], "USR");
        auto name = occurrence_detail::string_at(*strings, row[1], "name");
        auto kind = occurrence_detail::string_at(*strings, row[2], "kind");
        auto flags = occurrence_detail::integer(row[3], "flags");
        auto file = occurrence_detail::string_at(*strings, row[4], "file");
        auto line = occurrence_detail::integer(row[5], "line");
        auto column = occurrence_detail::integer(row[6], "column");
        auto declaration_file = occurrence_detail::string_at(
            *strings, row[7], "declaration file");
        auto declaration_line = occurrence_detail::integer(
            row[8], "declaration line");
        auto declaration_column = occurrence_detail::integer(
            row[9], "declaration column");
        if (!usr) return std::unexpected(usr.error());
        if (!name) return std::unexpected(name.error());
        if (!kind) return std::unexpected(kind.error());
        if (!flags) return std::unexpected(flags.error());
        if (!file) return std::unexpected(file.error());
        if (!line) return std::unexpected(line.error());
        if (!column) return std::unexpected(column.error());
        if (!declaration_file)
            return std::unexpected(declaration_file.error());
        if (!declaration_line)
            return std::unexpected(declaration_line.error());
        if (!declaration_column)
            return std::unexpected(declaration_column.error());
        constexpr auto maximum =
            std::int64_t(std::numeric_limits<unsigned>::max());
        if (*flags < 0 || *flags > 15 || *line < 1 || *column < 1 ||
            *declaration_line < 0 || *declaration_column < 0 ||
            *line > maximum || *column > maximum ||
            *declaration_line > maximum || *declaration_column > maximum)
            return std::unexpected(OccurrenceCodecError{
                "occurrence numeric field is out of range"});

        SymbolOccurrence occurrence;
        occurrence.usr = std::move(*usr);
        occurrence.name = std::move(*name);
        occurrence.kind = std::move(*kind);
        occurrence.is_decl = (*flags & 1) != 0;
        occurrence.is_def = (*flags & 2) != 0;
        occurrence.is_ref = (*flags & 4) != 0;
        occurrence.is_call = (*flags & 8) != 0;
        occurrence.file = std::move(*file);
        occurrence.line = static_cast<unsigned>(*line);
        occurrence.column = static_cast<unsigned>(*column);
        occurrence.decl_file = std::move(*declaration_file);
        occurrence.decl_line = static_cast<unsigned>(*declaration_line);
        occurrence.decl_column = static_cast<unsigned>(*declaration_column);
        result.push_back(std::move(occurrence));
    }
    return result;
}

}
