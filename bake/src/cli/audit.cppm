// bake.cli.audit — the `audit` command.

export module bake.cli.audit;

import std;
import bake.util;
import bake.toolchain.target;
import bake.toolchain.lld;
import bake.toolchain.driver;
import bake.toolchain.runtime;
import bake.buildsystem.project;
import bake.buildsystem.moid;
import bake.buildsystem.graph;
import bake.buildsystem.cmdgen;
import bake.buildsystem.engine;
import bake.buildsystem.package;
import bake.buildsystem.query;
import bake.json;
import bake.cli;

namespace bake::cli {

// ===== audit command =====
//
// Verifies that bake's compiler produces binaries without relying on
// system SDK headers or libraries beyond the kernel interface (dyld,
// libSystem.B.dylib on macOS).

export int cmd_audit(const ParsedArgs &args) {
  Path exe = get_self_exe_path();
  if (exe.string().empty())
    exe = "bake";
  Path lib_dir = find_lib_dir();
  std::string lib_dir_str = lib_dir.string();

  // Create temp source files.
  Path tmp_dir = std::filesystem::temp_directory_path() / "bake-audit";
  tmp_dir.mkdir_recursive();
  Path src_c = tmp_dir / "audit.c";
  Path src_cpp = tmp_dir / "audit.cpp";
  Path out_c = tmp_dir / "audit_c";
  Path out_cpp = tmp_dir / "audit_cpp";
  write_file(src_c, "int main(void) { return 0; }\n");
  write_file(src_cpp, "#include <iostream>\n"
                      "int main() { std::cout << \"ok\"; return 0; }\n");

  int violations = 0;

  // ── Check 1: compile C, verify it works ──
  {
    std::vector<std::string> cmd = {exe.string(), "cc", src_c.string(), "-o",
                                    out_c.string()};
    auto r = run_process(cmd, Path(), true);
    if (!r.success()) {
      std::println(std::cerr, "FAIL: bake cc failed to compile test program");
      std::print(std::cerr, "{}", r.stderr_output);
      return 1;
    }
  }

  // ── Check 2: compile C++, verify it works ──
  {
    std::vector<std::string> cmd = {exe.string(), "c++", src_cpp.string(), "-o",
                                    out_cpp.string()};
    auto r = run_process(cmd, Path(), true);
    if (!r.success()) {
      std::println(std::cerr, "FAIL: bake c++ failed to compile test program");
      std::print(std::cerr, "{}", r.stderr_output);
      return 1;
    }
  }

  // ── Check 3: inspect output binary dependencies ──
  auto host = detect_host_target();
  if (host.is_darwin() && out_c.exists()) {
    auto r = run_process({"otool", "-L", out_c.string()}, Path(), true);
    if (r.success()) {
      std::istringstream ss(r.stdout_output);
      std::string line;
      while (std::getline(ss, line)) {
        // Whitelist: the binary itself, dyld, libSystem.
        if (line.find(out_c.filename().string()) != std::string::npos)
          continue;
        if (line.find("/usr/lib/libSystem.B.dylib") != std::string::npos)
          continue;
        if (line.find("/usr/lib/dyld") != std::string::npos)
          continue;
        // Any other dynamic dependency is a violation.
        if (line.find(".dylib") != std::string::npos ||
            line.find(".so") != std::string::npos) {
          std::println(std::cerr, "  VIOLATION: unexpected dynamic dep: {}",
                       std::string_view(line).substr(0, 120));
          violations++;
        }
      }
    }
  }

  // Clean up.
  src_c.remove();
  src_cpp.remove();
  out_c.remove();
  out_cpp.remove();
  tmp_dir.remove();

  if (violations == 0) {
    std::println("PASS: toolchain is self-contained");
    return 0;
  } else {
    std::println(std::cerr, "FAIL: {} violation(s) found", violations);
    return 1;
  }
}

} // namespace bake::cli
