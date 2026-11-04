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

// pfsql: offline parser for PerfettoSQL source files. Does not load a trace.
// It reports what the SQL says as JSON and leaves deciding what is allowed to
// its callers, like tools/check_sql_modules.py.

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/status_or.h"
#include "src/tools/pfsql/source_tree.h"
#include "src/trace_processor/util/simple_json_serializer.h"
#include "src/trace_processor/util/sql_module_doc_parser.h"

namespace perfetto::pfsql {
namespace {

namespace json = trace_processor::json;
namespace doc = trace_processor::stdlib_doc;

const char* kHelp = R"(Usage: pfsql parse <tree_path>...

Parses trees of PerfettoSQL `.sql` files and prints a single JSON object with a
`modules` array, sorted by module name. All trees share one module namespace:
the path within a tree gives the module name (`slices/with_context.sql` is
`slices.with_context`) and the first tree wins on a collision.

Each module has:
  - module / path
  - includes:    the modules named by INCLUDE PERFETTO MODULE statements
  - symbols:     each table, view, function and macro defined, in source order,
                 with its name, kind and references
  - references:  names used by statements which define no symbol
  - mutations:   DROP and INSERT statements
  - uses_like:   whether the LIKE operator appears in the module
  - docs:        the tables, views, functions and macros created with CREATE
                 PERFETTO, with their comments, arguments, columns and return
                 types
  - errors:      parse errors

Macros are not expanded: references in a macro's body belong to the macro and
references in its arguments belong to the caller.
)";

template <typename Entry>
void WriteEntries(json::JsonDictSerializer& dict,
                  std::string_view key,
                  const std::vector<Entry>& entries) {
  dict.AddArray(key, [&](json::JsonArraySerializer& a) {
    for (const Entry& e : entries) {
      a.AppendDict([&](json::JsonDictSerializer& d) {
        d.AddString("name", e.name);
        d.AddString("type", e.type);
        d.AddString("description", e.description);
        d.AddBool("is_variadic", e.is_variadic);
      });
    }
  });
}

void WriteDocs(json::JsonDictSerializer& docs,
               const doc::ParsedModule& parsed) {
  docs.AddArray("table_views", [&](json::JsonArraySerializer& a) {
    for (const doc::TableOrView& t : parsed.table_views) {
      a.AppendDict([&](json::JsonDictSerializer& d) {
        d.AddString("name", t.name);
        d.AddString("type", t.type);
        d.AddString("description", t.raw_description);
        d.AddBool("or_replace", t.or_replace);
        WriteEntries(d, "columns", t.columns);
      });
    }
  });
  docs.AddArray("functions", [&](json::JsonArraySerializer& a) {
    for (const doc::Function& f : parsed.functions) {
      a.AppendDict([&](json::JsonDictSerializer& d) {
        d.AddString("name", f.name);
        d.AddString("description", f.raw_description);
        d.AddBool("or_replace", f.or_replace);
        d.AddBool("is_table_function", f.is_table_function);
        d.AddString("return_type", f.return_type);
        d.AddString("return_description", f.return_description);
        WriteEntries(d, "args", f.args);
        WriteEntries(d, "columns", f.columns);
      });
    }
  });
  docs.AddArray("macros", [&](json::JsonArraySerializer& a) {
    for (const doc::Macro& m : parsed.macros) {
      a.AppendDict([&](json::JsonDictSerializer& d) {
        d.AddString("name", m.name);
        d.AddString("description", m.raw_description);
        d.AddBool("or_replace", m.or_replace);
        d.AddString("return_type", m.return_type);
        d.AddString("return_description", m.return_description);
        WriteEntries(d, "args", m.args);
      });
    }
  });
}

std::string ToJson(const std::vector<Module>& modules) {
  auto write_strings = [](const std::vector<std::string>& items) {
    return [&items](json::JsonArraySerializer& a) {
      for (const std::string& s : items)
        a.AppendString(s);
    };
  };
  std::string out = json::SerializeJson([&](json::JsonValueSerializer&& w) {
    std::move(w).WriteDict([&](json::JsonDictSerializer& root) {
      root.AddArray("modules", [&](json::JsonArraySerializer& arr) {
        for (const Module& m : modules) {
          arr.AppendDict([&](json::JsonDictSerializer& mod) {
            mod.AddString("module", m.name);
            mod.AddString("path", m.path);
            mod.AddArray("includes", write_strings(m.includes));
            mod.AddArray("symbols", [&](json::JsonArraySerializer& syms) {
              for (const Symbol& s : m.symbols) {
                syms.AppendDict([&](json::JsonDictSerializer& sd) {
                  sd.AddString("name", s.name);
                  sd.AddString("kind", SymbolKindName(s.kind));
                  sd.AddString("form", SymbolFormName(s.form));
                  sd.AddArray("references", write_strings(s.references));
                });
              }
            });
            mod.AddArray("references", write_strings(m.references));
            mod.AddArray("mutations", [&](json::JsonArraySerializer& muts) {
              for (const Mutation& mut : m.mutations) {
                muts.AppendDict([&](json::JsonDictSerializer& md) {
                  md.AddString("kind", mut.kind);
                  md.AddString("object_type", mut.object_type);
                  md.AddString("name", mut.name);
                });
              }
            });
            mod.AddBool("uses_like", m.uses_like);
            std::string sql;
            doc::ParsedModule parsed;
            if (base::ReadFile(m.absolute_path, &sql)) {
              parsed = doc::ParseStdlibModule(
                  sql.data(), static_cast<uint32_t>(sql.size()));
            }
            mod.AddDict("docs", [&](json::JsonDictSerializer& docs) {
              WriteDocs(docs, parsed);
            });
            mod.AddArray("errors", write_strings(m.errors));
          });
        }
      });
    });
  });
  out.push_back('\n');
  return out;
}

int Main(int argc, char** argv) {
  std::string_view command = argc > 1 ? argv[1] : "";
  if (command == "-h" || command == "--help" || command == "help") {
    fputs(kHelp, stdout);
    return 0;
  }
  if (command != "parse" || argc < 3) {
    fputs(kHelp, stderr);
    return 1;
  }
  base::StatusOr<std::vector<Module>> modules =
      ParseSourceTrees(std::vector<std::string>(argv + 2, argv + argc));
  if (!modules.ok()) {
    fprintf(stderr, "pfsql parse: %s\n", modules.status().c_message());
    return 1;
  }
  std::string s = ToJson(*modules);
  fwrite(s.data(), 1, s.size(), stdout);
  return 0;
}

}  // namespace
}  // namespace perfetto::pfsql

int main(int argc, char** argv) {
  return perfetto::pfsql::Main(argc, argv);
}
