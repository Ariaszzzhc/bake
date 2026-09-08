// bake.cli.dep_edit — manifest TOML surgery shared by the add and
// remove commands (section-scoped dependency line insert/erase).

export module bake.cli.dep_edit;

import std;
import bake.util;
import bake.buildsystem.project;

namespace bake::cli {

// Find the start of the next table header ('[' at column 0) on or after
// `from`, or npos when the section runs to end of file.
export std::size_t next_table_header(const std::string &content, std::size_t from) {
  std::size_t pos = from;
  while (pos < content.size()) {
    std::size_t eol = content.find('\n', pos);
    if (eol == std::string::npos) eol = content.size();
    if (eol > pos && content[pos] == '[') return pos;
    if (eol == content.size()) break;
    pos = eol + 1;
  }
  return std::string::npos;
}

// Insert a dependency line at the end of a [dependencies] section (global
// or target-scoped), creating the section if needed. Existing entries,
// comments, and ordering are untouched.
export bool insert_dependency_line(const Path &toml_path, const std::string &section,
                            const std::string &dep_line) {
  auto content = read_file(toml_path);
  if (!content) return false;

  std::size_t header = content->find("\n" + section + "\n");
  if (header == std::string::npos && content->rfind(section + "\n", 0) == 0)
    header = 0;
  if (header != std::string::npos) {
    std::size_t section_line_end = content->find('\n', header + 1);
    if (section_line_end == std::string::npos) section_line_end = content->size();
    std::size_t next = next_table_header(*content, section_line_end + 1);
    std::size_t insert_at = (next == std::string::npos)
                                ? content->size()
                                : next;
    if (insert_at > 0 && (*content)[insert_at - 1] != '\n')
      content->insert(insert_at, "\n");
    content->insert(insert_at, dep_line);
  } else {
    if (!content->empty() && content->back() != '\n')
      *content += "\n";
    *content += "\n" + section + "\n" + dep_line;
  }
  return write_file(toml_path, *content);
}

// Remove the entry line for `alias` from a section. Returns false when the
// section or the entry is not present.
export bool remove_dependency_line(const Path &toml_path, const std::string &section,
                            const std::string &alias) {
  auto content = read_file(toml_path);
  if (!content) return false;

  std::size_t header = content->find("\n" + section + "\n");
  if (header == std::string::npos && content->rfind(section + "\n", 0) == 0)
    header = 0;
  if (header == std::string::npos) return false;
  std::size_t section_end =
      next_table_header(*content, content->find('\n', header + 1) + 1);
  if (section_end == std::string::npos) section_end = content->size();

  std::size_t pos = content->find('\n', header + 1) + 1;
  const std::string prefix = alias + " ";
  const std::string prefix_eq = alias + "=";
  while (pos < section_end) {
    std::size_t eol = content->find('\n', pos);
    if (eol == std::string::npos) eol = content->size();
    std::size_t line_start = pos;
    while (line_start < eol && (*content)[line_start] == ' ')
      ++line_start;
    std::string_view line(content->data() + line_start, eol - line_start);
    if (line.starts_with(prefix_eq) ||
        (line.starts_with(prefix) &&
         line.substr(prefix.size()).starts_with("="))) {
      content->erase(line_start, eol - line_start + (eol < content->size() ? 1 : 0));
      return write_file(toml_path, *content);
    }
    if (eol == content->size()) break;
    pos = eol + 1;
  }
  return false;
}

// Every scope of the manifest that can declare dependencies, as (label,
// table) pairs. The label is the exact TOML section header for edits.
export std::vector<std::pair<std::string, const std::map<std::string, Dependency> *>>
dependency_scopes(const Manifest &manifest) {
  std::vector<std::pair<std::string,
                        const std::map<std::string, Dependency> *>> scopes;
  scopes.emplace_back("[dependencies]", &manifest.dependencies);
  for (const auto &condition : manifest.targets)
    scopes.emplace_back(
        "[target.\"" + condition.triple_pattern + "\".dependencies]",
        &condition.dependencies);
  return scopes;
}

} // namespace bake::cli
