/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// pfsql: offline tool for working with PerfettoSQL source files. Does not
// load a trace. Each subcommand defines its own input shape.
//
// Subcommands:
//   lineage   Cross-module dependency graph over `.sql` trees.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/getopt.h"
#include "src/perfetto_sql/analysis/source_tree.h"
#include "src/trace_processor/util/json_value.h"
#include "src/trace_processor/util/simple_json_serializer.h"

namespace perfetto::pfsql {
namespace {

namespace json = trace_processor::json;
namespace analysis = ::perfetto::perfetto_sql::analysis;

// ---------- lineage subcommand ----------

std::string DirOf(const std::string& path) {
  auto slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

std::string ResolvePath(const std::string& base_dir, const std::string& p) {
  return (!p.empty() && p[0] == '/') ? p : base_dir + "/" + p;
}

base::Status LoadConfig(const std::string& config_path,
                        analysis::SourceTreeAnalyzer& analyzer) {
  std::string text;
  if (!base::ReadFile(config_path, &text))
    return base::ErrStatus("failed to read config %s", config_path.c_str());
  auto parsed = json::Parse(text);
  if (!parsed.ok())
    return base::ErrStatus("invalid JSON in %s: %s", config_path.c_str(),
                           parsed.status().c_message());
  const json::Dom& doc = *parsed;
  if (!doc.IsObject() || !doc.HasMember("trees") || !doc["trees"].IsArray())
    return base::ErrStatus("config must be { \"trees\": [ ... ] }");
  const std::string base_dir = DirOf(config_path);
  bool empty = true;
  for (const auto& t : doc["trees"]) {
    std::string root;
    if (t.IsString()) {
      root = t.AsString();
    } else if (t.IsObject() && t.HasMember("root") && t["root"].IsString()) {
      root = t["root"].AsString();
    } else {
      return base::ErrStatus(
          "each tree must be a string or { \"root\": \"...\" }");
    }
    analyzer.AddTree(ResolvePath(base_dir, root));
    empty = false;
  }
  return empty ? base::ErrStatus("config has no trees") : base::OkStatus();
}

struct SymbolRef {
  std::string_view name;
  analysis::SymbolKind kind;
};

using SymbolRefsByModule =
    base::FlatHashMap<std::string, std::vector<SymbolRef>>;
using SortedUseEntry = std::pair<std::string, const std::vector<SymbolRef>*>;

std::vector<SortedUseEntry> SortedUses(const SymbolRefsByModule& by_mod) {
  std::vector<SortedUseEntry> out;
  for (auto it = by_mod.GetIterator(); it; ++it)
    out.emplace_back(it.key(), &it.value());
  std::sort(out.begin(), out.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  return out;
}

std::vector<std::string_view> SortedUniqueStrings(
    std::vector<std::string_view> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

bool IsPrelude(std::string_view module) {
  constexpr std::string_view prefix = "prelude.";
  return module.size() >= prefix.size() &&
         module.substr(0, prefix.size()) == prefix;
}

SymbolRefsByModule UsesByModule(const analysis::Program& program,
                                const analysis::Symbol& symbol,
                                bool prelude) {
  SymbolRefsByModule out;
  for (const analysis::SymbolReference& reference : symbol.references) {
    const analysis::Symbol& target = program.symbol(reference.symbol_id);
    std::string_view module = program.module(target.module_id).name;
    if (IsPrelude(module) != prelude) {
      continue;
    }
    std::string key(module);
    std::vector<SymbolRef>* refs = out.Find(key);
    if (!refs) {
      out.Insert(key, {});
      refs = out.Find(key);
    }
    refs->push_back({target.name, target.kind});
  }
  return out;
}

std::vector<std::string_view> MissingIncludes(const analysis::Program& program,
                                              const analysis::Module& module) {
  std::vector<std::string_view> touched;
  for (analysis::SymbolId symbol_id : module.symbol_ids) {
    for (const analysis::SymbolReference& reference :
         program.symbol(symbol_id).references) {
      std::string_view target_module =
          program.module(program.symbol(reference.symbol_id).module_id).name;
      if (IsPrelude(target_module)) {
        continue;
      }
      bool declared =
          std::find(module.declared_includes.begin(),
                    module.declared_includes.end(),
                    target_module) != module.declared_includes.end();
      if (!declared) {
        touched.push_back(target_module);
      }
    }
  }
  return SortedUniqueStrings(std::move(touched));
}

std::vector<std::string_view> UnresolvedNames(const analysis::Symbol& symbol) {
  std::vector<std::string_view> out;
  out.reserve(symbol.unresolved_references.size());
  for (const analysis::UnresolvedReference& reference :
       symbol.unresolved_references) {
    out.push_back(reference.name);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::string EmitLineageJson(const analysis::Program& program) {
  auto write_strings = [](const auto& items) {
    return [&items](json::JsonArraySerializer& a) {
      for (const auto& s : items)
        a.AppendString(s);
    };
  };
  auto write_use_map = [](const SymbolRefsByModule& by_mod) {
    auto sorted = SortedUses(by_mod);
    return [sorted = std::move(sorted)](json::JsonDictSerializer& d) {
      for (const auto& kv : sorted) {
        d.AddArray(kv.first, [&kv](json::JsonArraySerializer& a) {
          for (const auto& u : *kv.second) {
            a.AppendDict([&u](json::JsonDictSerializer& e) {
              e.AddString("name", u.name);
              e.AddString("kind", analysis::SymbolKindName(u.kind));
            });
          }
        });
      }
    };
  };

  std::string out = json::SerializeJson([&](json::JsonValueSerializer&& w) {
    std::move(w).WriteDict([&](json::JsonDictSerializer& root) {
      root.AddArray("modules", [&](json::JsonArraySerializer& arr) {
        for (const analysis::Module& m : program.modules()) {
          arr.AppendDict([&](json::JsonDictSerializer& mod) {
            mod.AddString("module", m.name);
            mod.AddString("path", m.path);
            mod.AddArray("declared_includes",
                         write_strings(m.declared_includes));
            mod.AddArray("symbols", [&](json::JsonArraySerializer& syms) {
              for (analysis::SymbolId symbol_id : m.symbol_ids) {
                const analysis::Symbol& s = program.symbol(symbol_id);
                SymbolRefsByModule uses =
                    UsesByModule(program, s, /*prelude=*/false);
                SymbolRefsByModule implicit_uses =
                    UsesByModule(program, s, /*prelude=*/true);
                std::vector<std::string_view> unresolved = UnresolvedNames(s);
                syms.AppendDict([&](json::JsonDictSerializer& sd) {
                  sd.AddString("name", s.name);
                  sd.AddString("kind", analysis::SymbolKindName(s.kind));
                  sd.AddDict("uses", write_use_map(uses));
                  sd.AddDict("implicit_uses", write_use_map(implicit_uses));
                  sd.AddArray("intrinsics_or_external",
                              write_strings(unresolved));
                });
              }
            });
            std::vector<std::string_view> missing = MissingIncludes(program, m);
            mod.AddArray("missing_includes", write_strings(missing));
            mod.AddArray("errors", write_strings(m.diagnostics));
          });
        }
      });
    });
  });
  out.push_back('\n');
  return out;
}

const char* kLineageHelp = R"(Usage:
  pfsql lineage <tree_path>...
  pfsql lineage --config <file.json>

Cross-module dependency graph over PerfettoSQL `.sql` trees. Input is given
EITHER as positional tree paths (resolved against the CWD) OR as a JSON
config (not both). The JSON shape is:
  { "trees": [ "path/to/stdlib",
               { "root": "path/to/other" } ] }
Paths inside the JSON are resolved against the config's dir.

All trees share one module namespace; the path within each tree determines
the dotted module name (`slices/with_context.sql` -> `slices.with_context`).
First-tree-wins on name collisions.

Anything under `prelude.*` is treated as auto-included (the runtime engine
auto-loads it). Macros are expanded recursively — references inside macro
bodies surface in the resolved record of the invoking symbol.

Output: a single JSON object on stdout with a `modules` array, one entry per
module:
  - module / path
  - declared_includes:  authored INCLUDE PERFETTO MODULE stmts
  - symbols:            one entry per CREATE PERFETTO
                        TABLE/VIEW/FUNCTION/MACRO, in source order. Each
                        carries its OWN:
                          - name / kind
                          - uses:           cross-module refs by defining
                                            module
                          - implicit_uses:  refs into prelude.*
                          - intrinsics_or_external:
                                            bare names not defined anywhere
  - missing_includes:   non-prelude modules used by any symbol but not
                        declared
  - errors:             parse errors or symbol-name collisions
)";

int RunLineage(int argc, char** argv) {
  std::string config_path;
  static const option long_opts[] = {
      {"config", required_argument, nullptr, 'c'},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0}};
  optind = 1;  // restart getopt
  for (;;) {
    int c = getopt_long(argc, argv, "c:h", long_opts, nullptr);
    if (c < 0)
      break;
    if (c == 'c') {
      config_path = optarg;
    } else if (c == 'h') {
      fputs(kLineageHelp, stdout);
      return 0;
    } else {
      fputs(kLineageHelp, stderr);
      return 1;
    }
  }
  std::vector<std::string> tree_paths;
  for (int i = optind; i < argc; ++i)
    tree_paths.emplace_back(argv[i]);

  const bool have_config = !config_path.empty();
  const bool have_paths = !tree_paths.empty();
  if (have_config && have_paths) {
    fprintf(stderr,
            "pfsql lineage: pass either positional tree paths or --config, "
            "not both\n");
    return 1;
  }
  if (!have_config && !have_paths) {
    fprintf(stderr,
            "pfsql lineage: expected one or more tree paths, or --config "
            "FILE\n");
    return 1;
  }

  analysis::SourceTreeAnalyzer analyzer;
  if (have_config) {
    if (auto st = LoadConfig(config_path, analyzer); !st.ok()) {
      fprintf(stderr, "%s\n", st.c_message());
      return 1;
    }
  } else {
    for (const auto& p : tree_paths)
      analyzer.AddTree(p);
  }
  base::StatusOr<analysis::Program> program = analyzer.Analyze();
  if (!program.ok()) {
    fprintf(stderr, "pfsql lineage: %s\n", program.status().c_message());
    return 1;
  }
  std::string s = EmitLineageJson(*program);
  fwrite(s.data(), 1, s.size(), stdout);
  return 0;
}

// ---------- top-level dispatcher ----------

const char* kTopLevelHelp =
    R"(pfsql: offline tool for working with PerfettoSQL.

Usage: pfsql <subcommand> [args]

Subcommands:
  lineage   Cross-module dependency graph over `.sql` trees.

Run 'pfsql <subcommand> --help' for subcommand details.
)";

int Main(int argc, char** argv) {
  if (argc < 2) {
    fputs(kTopLevelHelp, stderr);
    return 1;
  }
  std::string_view sub(argv[1]);
  if (sub == "-h" || sub == "--help" || sub == "help") {
    fputs(kTopLevelHelp, stdout);
    return 0;
  }
  if (sub == "lineage") {
    // Shift argv so getopt sees `pfsql lineage [args]` as `lineage [args]`.
    return RunLineage(argc - 1, argv + 1);
  }
  fprintf(stderr, "pfsql: unknown subcommand '%.*s'\n\n",
          static_cast<int>(sub.size()), sub.data());
  fputs(kTopLevelHelp, stderr);
  return 1;
}

}  // namespace
}  // namespace perfetto::pfsql

int main(int argc, char** argv) {
  return perfetto::pfsql::Main(argc, argv);
}
