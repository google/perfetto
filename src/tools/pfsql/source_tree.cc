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

#include "src/tools/pfsql/source_tree.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/perfetto_sql/intrinsic_macro_expansion.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/perfetto_sql/syntaqlite/utils.h"

namespace perfetto::pfsql {
namespace {

namespace sql = ::perfetto::perfetto_sql;

// These stand in for what is not expanded: every macro call and `$variable`.
// Identifiers parse in all the positions either can appear in.
constexpr std::string_view kMacroCallStub = "__pfsql_macro_call";
constexpr std::string_view kVariableStub = "__pfsql_variable";

bool IsStub(std::string_view name) {
  return name.substr(0, 8) == "__pfsql_";
}

class NameSet {
 public:
  bool Insert(std::string name) {
    return names_.Insert(std::move(name), true).second;
  }

  bool Contains(std::string_view name) const {
    return names_.Find(std::string(name)) != nullptr;
  }

  base::FlatHashMap<std::string, bool>::Iterator GetIterator() const {
    return names_.GetIterator();
  }

 private:
  base::FlatHashMap<std::string, bool> names_;
};

// "slices/with_context.sql" → "slices.with_context".
std::string ModuleNameFromRelPath(const std::string& rel) {
  std::string r = rel;
  constexpr std::string_view kSuffix = ".sql";
  if (r.size() >= kSuffix.size() &&
      std::string_view(r).substr(r.size() - kSuffix.size()) == kSuffix) {
    r.resize(r.size() - kSuffix.size());
  }
  for (char& c : r) {
    if (c == '/')
      c = '.';
  }
  return r;
}

// References collected from a single definition.
struct RawRefs {
  NameSet relation_refs;
  NameSet function_refs;
  NameSet macro_invocations;
};

// A defined symbol as seen during parsing, before resolution.
struct ScratchSymbol {
  std::string name;
  SymbolKind kind;
  RawRefs refs;
};

struct ScratchModule {
  std::string module;
  std::string path;
  std::string tree_root;
  std::vector<ScratchSymbol> symbols;
  std::vector<std::string> includes;
  std::vector<std::string> errors;
  // References from statements which define no symbol.
  RawRefs refs;
};

struct DiscoveredFile {
  std::string module;
  std::string tree_root;
  std::string rel_path;
};

struct MacroDef {
  std::string body;
  std::string return_type;
  std::vector<std::string> param_names;
  std::vector<std::string> param_types;
};

class MacroRegistry {
 public:
  void Add(std::string name, MacroDef def) {
    defs_.Insert(std::move(name), std::move(def));
  }
  const MacroDef* Lookup(std::string_view name) const {
    return defs_.Find(std::string(name));
  }
  sql::IntrinsicMacroExpander& intrinsics() { return intrinsics_; }

 private:
  base::FlatHashMap<std::string, MacroDef> defs_;
  sql::IntrinsicMacroExpander intrinsics_;
};

struct ParserDeleter {
  void operator()(SyntaqliteParser* p) const { syntaqlite_parser_destroy(p); }
};
using ScopedParser = std::unique_ptr<SyntaqliteParser, ParserDeleter>;

std::string SpanString(SyntaqliteParser* p, SyntaqliteTextSpan span) {
  // Macro-arg drill-through can hand us spans with surrounding whitespace
  // (authored arg text is verbatim). Trim so identifiers compare cleanly.
  return std::string(base::TrimWhitespace(sql::SyntaqliteSpanText(p, span)));
}

// Reads a span as the tokenizer saw it, so a name inside a macro body is that
// name and not the macro's call site.
std::string ExpandedSpanString(SyntaqliteParser* p, SyntaqliteTextSpan span) {
  SyntaqliteLength len = 0;
  const char* text = syntaqlite_parser_span_expanded_text(p, &span, &len);
  return text ? std::string(base::TrimWhitespace(std::string_view(text, len)))
              : std::string();
}

ScopedParser MakeParser() {
  ScopedParser p(syntaqlite_parser_create_perfetto(nullptr));
  PERFETTO_CHECK(p != nullptr);
  syntaqlite_parser_set_macro_fallback(p.get(), 1);
  return p;
}

void ExtractMacroParams(SyntaqliteParser* p,
                        uint32_t arg_list_id,
                        std::vector<std::string>& names,
                        std::vector<std::string>& types) {
  if (!syntaqlite_node_is_present(arg_list_id))
    return;
  const auto* list = static_cast<const SyntaqlitePerfettoMacroArgList*>(
      syntaqlite_parser_node(p, arg_list_id));
  uint32_t count = syntaqlite_list_count(list);
  names.reserve(count);
  types.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t id = syntaqlite_list_child_id(list, i);
    if (!syntaqlite_node_is_present(id))
      continue;
    const auto* item = static_cast<const SyntaqlitePerfettoMacroArg*>(
        syntaqlite_parser_node(p, id));
    if (!item)
      continue;
    names.emplace_back(SpanString(p, item->arg_name));
    types.emplace_back(base::ToLower(SpanString(p, item->arg_type)));
  }
}

// Walk every node in the current statement's arena and harvest references
// into `out`. CTE-local names are filtered out via a scratch set.
void CollectStatementRefs(SyntaqliteParser* p,
                          RawRefs& out,
                          NameSet& cte_locals,
                          bool column_refs_are_relations = false) {
  uint32_t n = syntaqlite_parser_node_count(p);
  for (uint32_t id = 0; id < n; ++id) {
    if (!syntaqlite_node_is_present(id))
      continue;
    const auto* node =
        static_cast<const SyntaqliteNode*>(syntaqlite_parser_node(p, id));
    if (node && node->tag == SYNTAQLITE_NODE_CTE_DEFINITION) {
      cte_locals.Insert(ExpandedSpanString(p, node->cte_definition.cte_name));
    }
  }
  for (uint32_t id = 0; id < n; ++id) {
    if (!syntaqlite_node_is_present(id))
      continue;
    const auto* node =
        static_cast<const SyntaqliteNode*>(syntaqlite_parser_node(p, id));
    if (!node)
      continue;
    switch (static_cast<int>(node->tag)) {
      case SYNTAQLITE_NODE_TABLE_REF: {
        std::string name = ExpandedSpanString(p, node->table_ref.table_name);
        if (name.empty() || cte_locals.Contains(name))
          break;
        out.relation_refs.Insert(std::move(name));
        break;
      }
      case SYNTAQLITE_NODE_PERFETTO_PIPE_SOURCE: {
        std::string name =
            ExpandedSpanString(p, node->perfetto_pipe_source.table_name);
        if (!name.empty() && !cte_locals.Contains(name))
          out.relation_refs.Insert(std::move(name));
        break;
      }
      case SYNTAQLITE_NODE_CREATE_PERFETTO_INDEX_STMT:
        out.relation_refs.Insert(
            ExpandedSpanString(p, node->create_perfetto_index_stmt.table_name));
        break;
      case SYNTAQLITE_NODE_CREATE_PERFETTO_DELEGATING_FUNCTION_STMT:
        out.function_refs.Insert(ExpandedSpanString(
            p, node->create_perfetto_delegating_function_stmt.delegate_to));
        break;
      case SYNTAQLITE_NODE_CREATE_VIRTUAL_TABLE_STMT: {
        const auto& vtab = node->create_virtual_table_stmt;
        if (!base::StartsWith(
                base::ToUpper(ExpandedSpanString(p, vtab.module_name)),
                "SPAN_")) {
          break;
        }
        // The grammar keeps module arguments as raw text: `t PARTITIONED c, u`.
        for (const std::string& arg :
             base::SplitString(ExpandedSpanString(p, vtab.module_args), ",")) {
          std::vector<std::string> words =
              base::SplitString(std::string(base::TrimWhitespace(arg)), " ");
          if (!words.empty())
            out.relation_refs.Insert(words[0]);
        }
        break;
      }
      case SYNTAQLITE_NODE_COLUMN_REF: {
        if (!column_refs_are_relations)
          break;
        std::string name = ExpandedSpanString(p, node->column_ref.column);
        if (!name.empty() && !cte_locals.Contains(name))
          out.relation_refs.Insert(std::move(name));
        break;
      }
      case SYNTAQLITE_NODE_FUNCTION_CALL:
      case SYNTAQLITE_NODE_AGGREGATE_FUNCTION_CALL:
      case SYNTAQLITE_NODE_ORDERED_SET_FUNCTION_CALL: {
        SyntaqliteTextSpan span =
            node->tag == SYNTAQLITE_NODE_FUNCTION_CALL
                ? node->function_call.func_name
                : (node->tag == SYNTAQLITE_NODE_AGGREGATE_FUNCTION_CALL
                       ? node->aggregate_function_call.func_name
                       : node->ordered_set_function_call.func_name);
        std::string name = ExpandedSpanString(p, span);
        if (name.empty())
          break;
        out.function_refs.Insert(std::move(name));
        break;
      }
      default:
        break;
    }
  }
}

struct MacroCall {
  std::string name;
  std::vector<std::string> args;
};

// The macro lookup state of one parse.
struct MacroLookup {
  MacroRegistry* registry;
  std::vector<MacroCall>* calls;
};

int MacroLookupCb(void* user_data,
                  SyntaqliteParser* parser,
                  const char* name,
                  SyntaqliteLength name_len,
                  const SyntaqliteToken* args,
                  uint32_t arg_count) {
  auto* lookup = static_cast<MacroLookup*>(user_data);
  std::string_view nm(name, name_len);

  // Intrinsics paste their arguments together, so only their expansion shows
  // what those arguments mean. They need a literal list to expand, which a
  // `$variable` is not: those fall through and their arguments stay visible.
  std::string_view stub = kMacroCallStub;
  switch (lookup->registry->intrinsics().TryExpand(nm, args, arg_count)) {
    case sql::ExpandStatus::kExpanded: {
      std::string_view body = lookup->registry->intrinsics().body();
      syntaqlite_macro_expansion_set_result(
          parser, body.data(), static_cast<SyntaqliteLength>(body.size()), 0,
          0);
      return SYNTAQLITE_MACRO_LOOKUP_OK;
    }
    case sql::ExpandStatus::kExpansionFailed:
      // Prefix forms expand to `, a, b`, attaching to what precedes them.
      if (base::EndsWith(nm, "_prefix"))
        stub = {};
      break;
    case sql::ExpandStatus::kNotIntrinsic:
      break;
  }

  MacroCall call;
  call.name = std::string(nm);
  for (uint32_t i = 0; i < arg_count; ++i) {
    call.args.emplace_back(args[i].text, args[i].length);
  }
  lookup->calls->push_back(std::move(call));
  syntaqlite_macro_expansion_set_result(
      parser, stub.data(), static_cast<SyntaqliteLength>(stub.size()), 0, 0);
  return SYNTAQLITE_MACRO_LOOKUP_OK;
}

// Replaces each `$variable` with an identifier. `FROM $t` is not valid SQL
// until a macro parameter or runtime placeholder is substituted.
std::string StubVariables(std::string_view sql) {
  SyntaqliteTokenizer* tok = syntaqlite_tokenizer_create_perfetto(nullptr);
  PERFETTO_CHECK(tok != nullptr);
  syntaqlite_tokenizer_reset(tok, sql.data(),
                             static_cast<uint32_t>(sql.size()));
  std::string out;
  size_t copied = 0;
  SyntaqliteToken token;
  while (syntaqlite_tokenizer_next(tok, &token)) {
    if (token.type != SYNTAQLITE_TK_VARIABLE || token.text[0] != '$')
      continue;
    auto offset = static_cast<size_t>(token.text - sql.data());
    out.append(sql.substr(copied, offset - copied));
    out.append(kVariableStub);
    copied = offset + token.length;
  }
  out.append(sql.substr(copied));
  syntaqlite_tokenizer_destroy(tok);
  return out;
}

bool IsIdentifier(std::string_view s) {
  if (s.empty() || (s[0] >= '0' && s[0] <= '9'))
    return false;
  return std::all_of(s.begin(), s.end(), [](char c) {
    return c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z');
  });
}

// Collects the references of everything a statement's macro calls were handed,
// following calls nested in their arguments.
class RefCollector {
 public:
  RefCollector(MacroRegistry& registry, RawRefs& out)
      : registry_(registry), out_(out) {}

  void FlushCalls() {
    while (!calls_.empty()) {
      MacroCall call = std::move(calls_.back());
      calls_.pop_back();
      out_.macro_invocations.Insert(call.name);
      const MacroDef* def = registry_.Lookup(call.name);
      for (size_t i = 0; i < call.args.size(); ++i) {
        // A forwarded parameter or macro the registry doesn't know about says
        // nothing of how its arguments are used, so assume they name relations.
        std::string type = def && i < def->param_types.size()
                               ? def->param_types[i]
                               : std::string("tableorsubquery");
        CollectArg(call.args[i], type);
      }
    }
  }

  // Returns false if the body parses in none of the positions a macro can
  // expand in.
  bool CollectMacroBody(const MacroDef& def) {
    return TryWrappers(StubVariables(def.body), def.return_type,
                       /*column_refs_are_relations=*/false);
  }

  std::vector<MacroCall>& calls() { return calls_; }
  NameSet& cte_locals() { return cte_locals_; }
  const std::vector<std::string>& unparsed_args() const {
    return unparsed_args_;
  }

 private:
  void CollectArg(const std::string& arg, const std::string& type) {
    std::string_view trimmed = base::TrimWhitespace(std::string_view(arg));
    if (trimmed.empty() || trimmed == "()")
      return;
    bool names_relation = type == "tableorsubquery";
    bool names_relations = type == "_tablenamelist";
    if (IsIdentifier(trimmed)) {
      if ((names_relation || names_relations) &&
          !cte_locals_.Contains(trimmed)) {
        out_.relation_refs.Insert(std::string(trimmed));
      }
      return;
    }
    if (!TryWrappers(StubVariables(trimmed), type, names_relations)) {
      unparsed_args_.emplace_back(trimmed);
    }
  }

  // Parses `sql` in the position its type says it expands in, then in the
  // others: a forwarded argument's real type is unknown here.
  bool TryWrappers(const std::string& sql,
                   const std::string& type,
                   bool column_refs_are_relations) {
    const std::string table = "SELECT * FROM " + sql + ";";
    const std::string expr = "SELECT " + sql + ";";
    const std::string fragment = "SELECT " + sql + " FROM __pfsql_from;";
    std::vector<const std::string*> wrappers;
    if (type == "tableorsubquery") {
      wrappers = {&table, &expr, &fragment};
    } else if (type == "_projectionfragment") {
      wrappers = {&fragment, &expr, &table};
    } else {
      wrappers = {&expr, &fragment, &table};
    }
    for (const std::string* wrapper : wrappers) {
      if (TryParse(*wrapper, column_refs_are_relations))
        return true;
    }
    return false;
  }

  bool TryParse(const std::string& sql, bool column_refs_are_relations) {
    std::vector<MacroCall> calls;
    MacroLookup lookup{&registry_, &calls};
    ScopedParser owned = MakeParser();
    SyntaqliteParser* p = owned.get();
    syntaqlite_parser_set_macro_lookup(p, &MacroLookupCb, &lookup);
    syntaqlite_parser_reset(p, sql.data(), static_cast<uint32_t>(sql.size()));
    RawRefs refs;
    for (;;) {
      int32_t rc = syntaqlite_parser_next(p);
      if (rc == SYNTAQLITE_PARSE_DONE)
        break;
      if (rc == SYNTAQLITE_PARSE_ERROR)
        return false;
      CollectStatementRefs(p, refs, cte_locals_, column_refs_are_relations);
    }
    Merge(refs);
    for (MacroCall& call : calls) {
      calls_.push_back(std::move(call));
    }
    FlushCalls();
    return true;
  }

  void Merge(const RawRefs& refs) {
    for (auto it = refs.relation_refs.GetIterator(); it; ++it)
      out_.relation_refs.Insert(it.key());
    for (auto it = refs.function_refs.GetIterator(); it; ++it)
      out_.function_refs.Insert(it.key());
    for (auto it = refs.macro_invocations.GetIterator(); it; ++it)
      out_.macro_invocations.Insert(it.key());
  }

  MacroRegistry& registry_;
  RawRefs& out_;
  std::vector<MacroCall> calls_;
  NameSet cte_locals_;
  std::vector<std::string> unparsed_args_;
};

base::Status DiscoverTree(const std::string& root,
                          std::vector<DiscoveredFile>& out) {
  std::string trimmed = root;
  while (!trimmed.empty() && trimmed.back() == '/')
    trimmed.pop_back();
  std::vector<std::string> all;
  if (auto st = base::ListFilesRecursive(trimmed, all); !st.ok())
    return st;
  for (auto& rel : all) {
    if (rel.size() < 4 || rel.compare(rel.size() - 4, 4, ".sql") != 0)
      continue;
    out.push_back({ModuleNameFromRelPath(rel), trimmed, std::move(rel)});
  }
  return base::OkStatus();
}

// Pass A: walk every file once to collect macro bodies (for the expansion
// registry) and all top-level defined-symbol names (for the global symbol
// index). We do NOT call CollectStatementRefs here — refs are gathered in
// pass B once macros can be expanded.
void PassA(const DiscoveredFile& f, ScratchModule& m, MacroRegistry& registry) {
  m.module = f.module;
  m.path = f.rel_path;
  m.tree_root = f.tree_root;

  std::string sql;
  if (!base::ReadFile(f.tree_root + "/" + f.rel_path, &sql)) {
    m.errors.push_back("failed to read file");
    return;
  }
  ScopedParser owned = MakeParser();
  SyntaqliteParser* p = owned.get();
  syntaqlite_parser_reset(p, sql.data(), static_cast<uint32_t>(sql.size()));

  for (;;) {
    int32_t rc = syntaqlite_parser_next(p);
    if (rc == SYNTAQLITE_PARSE_DONE)
      break;
    if (rc == SYNTAQLITE_PARSE_ERROR) {
      const char* msg = syntaqlite_result_error_msg(p);
      m.errors.push_back(msg ? msg : "unknown parse error");
      continue;
    }
    uint32_t root = syntaqlite_result_root(p);
    if (!syntaqlite_node_is_present(root))
      continue;
    const auto* node =
        static_cast<const SyntaqliteNode*>(syntaqlite_parser_node(p, root));
    if (!node)
      continue;
    auto push_symbol = [&](std::string name, SymbolKind kind) {
      ScratchSymbol s;
      s.name = std::move(name);
      s.kind = kind;
      m.symbols.push_back(std::move(s));
    };
    switch (static_cast<int>(node->tag)) {
      case SYNTAQLITE_NODE_CREATE_TABLE_STMT:
        push_symbol(SpanString(p, node->create_table_stmt.table_name),
                    SymbolKind::kTable);
        break;
      case SYNTAQLITE_NODE_CREATE_VIRTUAL_TABLE_STMT:
        push_symbol(SpanString(p, node->create_virtual_table_stmt.table_name),
                    SymbolKind::kTable);
        break;
      case SYNTAQLITE_NODE_INCLUDE_PERFETTO_MODULE_STMT:
        m.includes.push_back(
            SpanString(p, node->include_perfetto_module_stmt.module_name));
        break;
      case SYNTAQLITE_NODE_CREATE_PERFETTO_TABLE_STMT:
        push_symbol(SpanString(p, node->create_perfetto_table_stmt.table_name),
                    SymbolKind::kTable);
        break;
      case SYNTAQLITE_NODE_CREATE_PERFETTO_VIEW_STMT:
        push_symbol(SpanString(p, node->create_perfetto_view_stmt.view_name),
                    SymbolKind::kView);
        break;
      case SYNTAQLITE_NODE_CREATE_PERFETTO_FUNCTION_STMT:
        push_symbol(
            SpanString(p, node->create_perfetto_function_stmt.function_name),
            SymbolKind::kFunction);
        break;
      case SYNTAQLITE_NODE_CREATE_PERFETTO_DELEGATING_FUNCTION_STMT:
        push_symbol(
            SpanString(
                p,
                node->create_perfetto_delegating_function_stmt.function_name),
            SymbolKind::kFunction);
        break;
      case SYNTAQLITE_NODE_CREATE_PERFETTO_MACRO_STMT: {
        const auto& n = node->create_perfetto_macro_stmt;
        std::string name = SpanString(p, n.macro_name);
        push_symbol(name, SymbolKind::kMacro);
        MacroDef def;
        def.body = SpanString(p, n.body);
        def.return_type = base::ToLower(SpanString(p, n.return_type));
        ExtractMacroParams(p, n.args, def.param_names, def.param_types);
        registry.Add(std::move(name), std::move(def));
        break;
      }
      default:
        break;
    }
  }
}

// Pass B: collects the references of each symbol. Macros are not expanded:
// calls are recorded and their arguments analyzed as SQL of their own, while
// each macro's body is analyzed on behalf of that macro. Statements are matched
// by source-order index against the symbols collected in pass A.
void PassB(const DiscoveredFile& f, MacroRegistry& registry, ScratchModule& m) {
  std::string sql;
  if (!base::ReadFile(f.tree_root + "/" + f.rel_path, &sql)) {
    m.errors.push_back("failed to read file");
    return;
  }
  std::vector<MacroCall> calls;
  MacroLookup lookup{&registry, &calls};
  ScopedParser owned = MakeParser();
  SyntaqliteParser* p = owned.get();
  syntaqlite_parser_set_macro_lookup(p, &MacroLookupCb, &lookup);
  syntaqlite_parser_reset(p, sql.data(), static_cast<uint32_t>(sql.size()));

  size_t sym_idx = 0;
  for (;;) {
    int32_t rc = syntaqlite_parser_next(p);
    if (rc == SYNTAQLITE_PARSE_DONE)
      break;
    if (rc == SYNTAQLITE_PARSE_ERROR) {
      // Already reported by pass A, which parses the same text.
      calls.clear();
      continue;
    }
    uint32_t root = syntaqlite_result_root(p);
    const auto* node = syntaqlite_node_is_present(root)
                           ? static_cast<const SyntaqliteNode*>(
                                 syntaqlite_parser_node(p, root))
                           : nullptr;
    if (!node) {
      calls.clear();
      continue;
    }

    RawRefs* refs = &m.refs;
    switch (static_cast<int>(node->tag)) {
      case SYNTAQLITE_NODE_CREATE_PERFETTO_TABLE_STMT:
      case SYNTAQLITE_NODE_CREATE_PERFETTO_VIEW_STMT:
      case SYNTAQLITE_NODE_CREATE_PERFETTO_FUNCTION_STMT:
      case SYNTAQLITE_NODE_CREATE_PERFETTO_DELEGATING_FUNCTION_STMT:
      case SYNTAQLITE_NODE_CREATE_PERFETTO_MACRO_STMT:
      case SYNTAQLITE_NODE_CREATE_TABLE_STMT:
      case SYNTAQLITE_NODE_CREATE_VIRTUAL_TABLE_STMT:
        if (sym_idx < m.symbols.size())
          refs = &m.symbols[sym_idx].refs;
        ++sym_idx;
        break;
      default:
        break;
    }

    RefCollector collector(registry, *refs);
    collector.calls().swap(calls);
    CollectStatementRefs(p, *refs, collector.cte_locals());
    collector.FlushCalls();
    if (node->tag == SYNTAQLITE_NODE_CREATE_PERFETTO_MACRO_STMT) {
      std::string name =
          SpanString(p, node->create_perfetto_macro_stmt.macro_name);
      const MacroDef* def = registry.Lookup(name);
      if (def && !collector.CollectMacroBody(*def)) {
        m.errors.push_back("failed to parse body of macro " + name);
      }
    }
    for (const std::string& arg : collector.unparsed_args()) {
      m.errors.push_back("failed to parse macro argument: " + arg);
    }
  }
}

std::vector<std::string> SortedReferences(const RawRefs& refs) {
  std::vector<std::string> names;
  for (const NameSet* set :
       {&refs.relation_refs, &refs.function_refs, &refs.macro_invocations}) {
    for (auto it = set->GetIterator(); it; ++it) {
      if (!IsStub(it.key()))
        names.push_back(it.key());
    }
  }
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  return names;
}

}  // namespace

const char* SymbolKindName(SymbolKind kind) {
  switch (kind) {
    case SymbolKind::kTable:
      return "table";
    case SymbolKind::kView:
      return "view";
    case SymbolKind::kFunction:
      return "function";
    case SymbolKind::kMacro:
      return "macro";
  }
  PERFETTO_FATAL("Unknown PerfettoSQL symbol kind");
}

base::StatusOr<std::vector<Module>> ParseSourceTrees(
    const std::vector<std::string>& roots) {
  std::vector<DiscoveredFile> files;
  for (const std::string& root : roots) {
    RETURN_IF_ERROR(DiscoverTree(root, files));
  }
  std::sort(files.begin(), files.end(),
            [](const DiscoveredFile& a, const DiscoveredFile& b) {
              return a.module < b.module;
            });
  {
    std::vector<DiscoveredFile> deduped;
    NameSet seen;
    for (auto& f : files) {
      if (seen.Insert(f.module))
        deduped.push_back(std::move(f));
    }
    files = std::move(deduped);
  }

  MacroRegistry registry;
  std::vector<ScratchModule> scratch(files.size());
  for (size_t i = 0; i < files.size(); ++i)
    PassA(files[i], scratch[i], registry);

  std::vector<Module> modules;
  modules.reserve(scratch.size());
  for (size_t i = 0; i < files.size(); ++i) {
    PassB(files[i], registry, scratch[i]);
    ScratchModule& m = scratch[i];
    Module module;
    module.name = std::move(m.module);
    module.path = std::move(m.path);
    module.includes = std::move(m.includes);
    module.references = SortedReferences(m.refs);
    module.errors = std::move(m.errors);
    for (ScratchSymbol& symbol : m.symbols) {
      module.symbols.push_back(
          {std::move(symbol.name), symbol.kind, SortedReferences(symbol.refs)});
    }
    modules.push_back(std::move(module));
  }
  return modules;
}

}  // namespace perfetto::pfsql
