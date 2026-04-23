/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/parser/perfetto_sql_parser.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/perfetto_sql/parser/function_util.h"
#include "src/trace_processor/perfetto_sql/preprocessor/perfetto_sql_preprocessor.h"
#include "src/trace_processor/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/util/sql_argument.h"

namespace perfetto::trace_processor {

namespace {

using Statement = PerfettoSqlParser::Statement;

std::string SpanText(SyntaqliteParser* p, SyntaqliteTextSpan span) {
  uint32_t len;
  const char* text = syntaqlite_parser_span_expanded_text(p, &span, &len);
  PERFETTO_CHECK(text != nullptr);
  return {text, len};
}

// Slices |stmt| using a source-layer span whose offset/length are direct byte
// offsets into the input string (layer_id == 0, injected by our grammar marker
// rules).
SqlSource SpanSource(const SqlSource& stmt, SyntaqliteTextSpan span) {
  PERFETTO_DCHECK(span._layer_id == 0);
  return stmt.Substr(span.offset, span.length);
}

base::StatusOr<std::vector<sql_argument::ArgumentDefinition>> BuildArgDefs(
    SyntaqliteParser* p,
    uint32_t list_id) {
  std::vector<sql_argument::ArgumentDefinition> result;
  if (!syntaqlite_node_is_present(list_id))
    return result;

  const auto* list = static_cast<const SyntaqlitePerfettoArgDefList*>(
      syntaqlite_parser_node(p, list_id));
  uint32_t count = syntaqlite_list_count(list);
  for (uint32_t i = 0; i < count; i++) {
    const auto* item = static_cast<const SyntaqlitePerfettoArgDef*>(
        syntaqlite_list_child(p, list, i));

    const auto* name_node = static_cast<const SyntaqliteNode*>(
        syntaqlite_parser_node(p, item->arg_name));
    std::string name = "$" + SpanText(p, name_node->ident_name.source);

    std::string type_str = SpanText(p, item->arg_type);
    // For JOINID(table.col) syntax, strip the hint suffix before type lookup.
    auto paren = type_str.find('(');
    if (paren != std::string::npos)
      type_str = type_str.substr(0, paren);
    auto type = sql_argument::ParseType(base::StringView(type_str));
    if (!type)
      return base::ErrStatus("Unknown argument type: %s", type_str.c_str());

    bool is_variadic = item->is_variadic == SYNTAQLITE_BOOL_TRUE;
    result.emplace_back(std::move(name), *type, is_variadic);
  }
  return result;
}

base::StatusOr<PerfettoSqlParser::CreateFunction::Returns> BuildReturnType(
    SyntaqliteParser* p,
    uint32_t rt_id) {
  const auto* rt = static_cast<const SyntaqlitePerfettoReturnType*>(
      syntaqlite_parser_node(p, rt_id));

  PerfettoSqlParser::CreateFunction::Returns result;
  if (rt->kind == SYNTAQLITE_PERFETTO_RETURN_KIND_TABLE) {
    result.is_table = true;
    auto cols = BuildArgDefs(p, rt->table_columns);
    if (!cols.ok())
      return cols.status();
    result.table_columns = std::move(*cols);
  } else {
    result.is_table = false;
    std::string type_str = SpanText(p, rt->scalar_type);
    auto type = sql_argument::ParseType(base::StringView(type_str));
    if (!type)
      return base::ErrStatus("Unknown return type: %s", type_str.c_str());
    result.scalar_type = *type;
  }
  return result;
}

}  // namespace

struct PerfettoSqlParser::Impl {
  explicit Impl(SqlSource source,
                const base::FlatHashMap<std::string,
                                        PerfettoSqlPreprocessor::Macro>& macros)
      : preprocessor(std::move(source), macros) {
    synq = syntaqlite_parser_create_with_dialect(nullptr,
                                                 syntaqlite_perfetto_dialect());
    PERFETTO_CHECK(synq != nullptr);
  }

  ~Impl() { syntaqlite_parser_destroy(synq); }

  SyntaqliteParser* synq;
  PerfettoSqlPreprocessor preprocessor;
  base::Status status;
  std::optional<Statement> current_statement;
};

PerfettoSqlParser::PerfettoSqlParser(
    SqlSource source,
    const base::FlatHashMap<std::string, PerfettoSqlPreprocessor::Macro>&
        macros)
    : impl_(std::make_unique<Impl>(std::move(source), macros)) {}

PerfettoSqlParser::~PerfettoSqlParser() = default;

bool PerfettoSqlParser::Next() {
  PERFETTO_DCHECK(impl_->status.ok());

  impl_->current_statement = std::nullopt;
  statement_sql_ = std::nullopt;

  if (!impl_->preprocessor.NextStatement()) {
    impl_->status = impl_->preprocessor.status();
    return false;
  }

  const SqlSource& stmt = impl_->preprocessor.statement();
  statement_sql_ = stmt;

  syntaqlite_parser_reset(impl_->synq, stmt.sql().data(),
                          static_cast<uint32_t>(stmt.sql().size()));

  int32_t rc = syntaqlite_parser_next(impl_->synq);
  if (rc == SYNTAQLITE_PARSE_DONE) {
    impl_->current_statement = SqliteSql{};
    return true;
  }
  if (rc == SYNTAQLITE_PARSE_ERROR) {
    uint32_t off = syntaqlite_result_error_offset(impl_->synq);
    impl_->status = base::ErrStatus("%s%s", stmt.AsTraceback(off).c_str(),
                                    syntaqlite_result_error_msg(impl_->synq));
    return false;
  }

  uint32_t root = syntaqlite_result_root(impl_->synq);
  const auto* node = static_cast<const SyntaqliteNode*>(
      syntaqlite_parser_node(impl_->synq, root));

  // Cast to int to suppress -Wswitch-enum: we intentionally handle only
  // Perfetto-dialect node types; all SQLite statement types fall through to the
  // default case and are returned as SqliteSql{}.
  switch (static_cast<int>(node->tag)) {
    case SYNTAQLITE_NODE_CREATE_PERFETTO_TABLE_STMT: {
      const auto& n = node->create_perfetto_table_stmt;
      std::string name = SpanText(impl_->synq, n.table_name);

      if (syntaqlite_node_is_present(n.table_impl)) {
        const auto* impl_node = static_cast<const SyntaqlitePerfettoTableImpl*>(
            syntaqlite_parser_node(impl_->synq, n.table_impl));
        std::string impl_name = SpanText(impl_->synq, impl_node->name);
        if (!base::CaseInsensitiveEqual(impl_name, "dataframe")) {
          impl_->status = base::ErrStatus("Invalid table implementation '%s'",
                                          impl_name.c_str());
          return false;
        }
      }

      auto schema = BuildArgDefs(impl_->synq, n.schema);
      if (!schema.ok()) {
        impl_->status = schema.status();
        return false;
      }
      impl_->current_statement = CreateTable{
          n.or_replace == SYNTAQLITE_BOOL_TRUE,
          std::move(name),
          std::move(*schema),
          SpanSource(stmt, n.select_span),
      };
      return true;
    }

    case SYNTAQLITE_NODE_CREATE_PERFETTO_VIEW_STMT: {
      const auto& n = node->create_perfetto_view_stmt;
      std::string name = SpanText(impl_->synq, n.view_name);

      auto schema = BuildArgDefs(impl_->synq, n.schema);
      if (!schema.ok()) {
        impl_->status = schema.status();
        return false;
      }
      SqlSource select_sql = SpanSource(stmt, n.select_span);

      SqlSource header = SqlSource::FromTraceProcessorImplementation(
          "CREATE VIEW " + name + " AS ");
      SqlSource::Rewriter rewriter(stmt);
      rewriter.Rewrite(0, n.select_span.offset, std::move(header));
      SqlSource create_view_sql = std::move(rewriter).Build();

      impl_->current_statement = CreateView{
          n.or_replace == SYNTAQLITE_BOOL_TRUE,
          std::move(name),
          std::move(*schema),
          std::move(select_sql),
          std::move(create_view_sql),
      };
      return true;
    }

    case SYNTAQLITE_NODE_CREATE_PERFETTO_FUNCTION_STMT: {
      const auto& n = node->create_perfetto_function_stmt;
      std::string name = SpanText(impl_->synq, n.function_name);

      auto args = BuildArgDefs(impl_->synq, n.args);
      if (!args.ok()) {
        impl_->status = args.status();
        return false;
      }
      // Variadic arguments are not allowed in SQL functions.
      for (const auto& arg : *args) {
        if (arg.is_variadic()) {
          impl_->status = base::ErrStatus(
              "Variadic arguments are only allowed in delegate functions (use "
              "DELEGATES TO instead of AS)");
          return false;
        }
      }

      auto returns = BuildReturnType(impl_->synq, n.return_type);
      if (!returns.ok()) {
        impl_->status = returns.status();
        return false;
      }
      impl_->current_statement = CreateFunction{
          n.or_replace == SYNTAQLITE_BOOL_TRUE,
          FunctionPrototype{std::move(name), std::move(*args)},
          std::move(*returns),
          SpanSource(stmt, n.select_span),
          "",
          std::nullopt,
      };
      return true;
    }

    case SYNTAQLITE_NODE_CREATE_PERFETTO_DELEGATING_FUNCTION_STMT: {
      const auto& n = node->create_perfetto_delegating_function_stmt;
      std::string name = SpanText(impl_->synq, n.function_name);

      auto args = BuildArgDefs(impl_->synq, n.args);
      if (!args.ok()) {
        impl_->status = args.status();
        return false;
      }
      // Variadic argument, if present, must be the last in the list.
      for (uint32_t i = 0; i + 1 < args->size(); ++i) {
        if ((*args)[i].is_variadic()) {
          impl_->status =
              base::ErrStatus("Variadic argument must be the last argument");
          return false;
        }
      }
      auto returns = BuildReturnType(impl_->synq, n.return_type);
      if (!returns.ok()) {
        impl_->status = returns.status();
        return false;
      }
      std::string delegate_to = SpanText(impl_->synq, n.delegate_to);
      impl_->current_statement = CreateFunction{
          n.or_replace == SYNTAQLITE_BOOL_TRUE,
          FunctionPrototype{std::move(name), std::move(*args)},
          std::move(*returns),
          SqlSource::FromTraceProcessorImplementation(""),
          "",
          std::move(delegate_to),
      };
      return true;
    }

    case SYNTAQLITE_NODE_CREATE_PERFETTO_INDEX_STMT: {
      const auto& n = node->create_perfetto_index_stmt;
      std::string index_name = SpanText(impl_->synq, n.index_name);
      std::string table_name = SpanText(impl_->synq, n.table_name);

      std::vector<std::string> col_names;
      if (syntaqlite_node_is_present(n.columns)) {
        const auto* list =
            static_cast<const SyntaqlitePerfettoIndexedColumnList*>(
                syntaqlite_parser_node(impl_->synq, n.columns));
        uint32_t count = syntaqlite_list_count(list);
        for (uint32_t i = 0; i < count; i++) {
          const auto* col = static_cast<const SyntaqlitePerfettoIndexedColumn*>(
              syntaqlite_list_child(impl_->synq, list, i));
          col_names.push_back(SpanText(impl_->synq, col->column_name));
        }
      }
      impl_->current_statement = CreateIndex{
          n.or_replace == SYNTAQLITE_BOOL_TRUE,
          std::move(index_name),
          std::move(table_name),
          std::move(col_names),
      };
      return true;
    }

    case SYNTAQLITE_NODE_CREATE_PERFETTO_MACRO_STMT: {
      const auto& n = node->create_perfetto_macro_stmt;

      // Build macro argument list as (name SqlSource, type SqlSource) pairs.
      std::vector<std::pair<SqlSource, SqlSource>> macro_args;
      if (syntaqlite_node_is_present(n.args)) {
        const auto* list = static_cast<const SyntaqlitePerfettoMacroArgList*>(
            syntaqlite_parser_node(impl_->synq, n.args));
        uint32_t count = syntaqlite_list_count(list);
        for (uint32_t i = 0; i < count; i++) {
          const auto* arg = static_cast<const SyntaqlitePerfettoMacroArg*>(
              syntaqlite_list_child(impl_->synq, list, i));
          std::string arg_name_str = SpanText(impl_->synq, arg->arg_name);
          std::string arg_type_str = SpanText(impl_->synq, arg->arg_type);
          macro_args.emplace_back(SqlSource::FromTraceProcessorImplementation(
                                      std::move(arg_name_str)),
                                  SqlSource::FromTraceProcessorImplementation(
                                      std::move(arg_type_str)));
        }
      }

      std::string macro_name_str = SpanText(impl_->synq, n.macro_name);
      std::string returns_str = SpanText(impl_->synq, n.return_type);
      std::string body_str = SpanText(impl_->synq, n.body);
      impl_->current_statement = CreateMacro{
          n.or_replace == SYNTAQLITE_BOOL_TRUE,
          SqlSource::FromTraceProcessorImplementation(
              std::move(macro_name_str)),
          std::move(macro_args),
          SqlSource::FromTraceProcessorImplementation(std::move(returns_str)),
          SqlSource::FromTraceProcessorImplementation(std::move(body_str)),
      };
      return true;
    }

    case SYNTAQLITE_NODE_INCLUDE_PERFETTO_MODULE_STMT: {
      const auto& n = node->include_perfetto_module_stmt;
      impl_->current_statement = Include{SpanText(impl_->synq, n.module_name)};
      return true;
    }

    case SYNTAQLITE_NODE_DROP_PERFETTO_INDEX_STMT: {
      const auto& n = node->drop_perfetto_index_stmt;
      impl_->current_statement = DropIndex{
          SpanText(impl_->synq, n.index_name),
          SpanText(impl_->synq, n.table_name),
      };
      return true;
    }

    default:
      // Any other SQLite statement passes through as SqliteSql.
      impl_->current_statement = SqliteSql{};
      return true;
  }
}

const PerfettoSqlParser::Statement& PerfettoSqlParser::statement() const {
  PERFETTO_DCHECK(impl_->current_statement.has_value());
  return *impl_->current_statement;
}

const base::Status& PerfettoSqlParser::status() const {
  return impl_->status;
}

}  // namespace perfetto::trace_processor
