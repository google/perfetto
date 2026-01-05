/*
 * Copyright (C) 2025 The Android Open Source Project
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

%name PerfettoSqlParse
%token_prefix TK_
%start_symbol input

%include {
#include <stdio.h>
#include <stddef.h>
#include <string>
#include <vector>
#include <memory>
#include <utility>
#include <optional>
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/perfetto_sql/grammar/perfettosql_grammar_interface.h"
#include "src/trace_processor/perfetto_sql/parser/perfetto_sql_parser.h"
#include "src/trace_processor/perfetto_sql/parser/function_util.h"
#include "src/trace_processor/perfetto_sql/grammar/perfettosql_parser_state.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/util/sql_argument.h"

#define YYNOERRORRECOVERY 1
#define YYPARSEFREENEVERNULL 1
}

%token CREATE REPLACE PERFETTO MACRO INCLUDE MODULE RETURNS FUNCTION DELEGATES.

%left OR.
%left AND.
%right NOT.
%left IS MATCH LIKE_KW BETWEEN IN ISNULL NOTNULL NE EQ.
%left GT LE LT GE.
%right ESCAPE.
%left BITAND BITOR LSHIFT RSHIFT.
%left PLUS MINUS.
%left STAR SLASH REM.
%left CONCAT PTR.
%left COLLATE.
%right BITNOT.
%nonassoc ON.

%fallback
// Taken from SQLite
  ID
  ABORT ACTION AFTER ANALYZE ASC ATTACH BEFORE BEGIN BY CASCADE CAST COLUMNKW
  CONFLICT DATABASE DEFERRED DESC DETACH DO
  EACH END EXCLUSIVE EXPLAIN FAIL FOR
  IGNORE IMMEDIATE INITIALLY INSTEAD LIKE_KW MATCH NO PLAN
  QUERY KEY OF OFFSET PRAGMA RAISE RECURSIVE RELEASE REPLACE RESTRICT ROW ROWS
  ROLLBACK SAVEPOINT TEMP TRIGGER VACUUM VIEW VIRTUAL WITH WITHOUT
  NULLS FIRST LAST
  CURRENT FOLLOWING PARTITION PRECEDING RANGE UNBOUNDED
  EXCLUDE GROUPS OTHERS TIES
  GENERATED ALWAYS
  MATERIALIZED
  REINDEX RENAME CTIME_KW IF
// Our additions.
  FUNCTION MODULE PERFETTO
  .
%wildcard ANY.

%token_type {struct PerfettoSqlToken}

%extra_context {struct PerfettoSqlParserState* state}
%syntax_error {
  OnPerfettoSqlSyntaxError(state, &yyminor);
}

// Helper function like scantok but usable by us.
pscantok(A) ::= . {
  assert( yyLookahead!=YYNOCODE );
  A = yyLookaheadToken;
}

// Shared rules
%type sql_argument_list { struct PerfettoSqlArgumentList* }
%destructor sql_argument_list { delete $$; }
sql_argument_list(A) ::=. { A = 0; }
sql_argument_list(A) ::= sql_argument_list_nonempty(X). { A = X; }

sql_argument_type(A) ::= ID(B). { A = B; }
sql_argument_type(A) ::= ID(B) LP ID DOT ID RP. { A = B; }

%type sql_argument_list_nonempty { struct PerfettoSqlArgumentList* }
%destructor sql_argument_list_nonempty { delete $$; }
sql_argument_list_nonempty(A) ::= sql_argument_list_nonempty(B) COMMA ID(C) sql_argument_type(D). {
  auto parsed = OnPerfettoSqlParseType(D);
  if (!parsed) {
    OnPerfettoSqlError(state, "Failed to parse type", D);
    delete B;
    A = nullptr;
  } else {
    B->inner.emplace_back("$" + std::string(C.ptr, C.n), *parsed);
    A = B;
  }
}
sql_argument_list_nonempty(A) ::= ID(B) sql_argument_type(C). {
  auto parsed = OnPerfettoSqlParseType(C);
  if (!parsed) {
    OnPerfettoSqlError(state, "Failed to parse type", C);
    A = nullptr;
  } else {
    A = new PerfettoSqlArgumentList();
    A->inner.emplace_back("$" + std::string(B.ptr, B.n), *parsed);
  }
}

%type table_schema { struct PerfettoSqlArgumentList* }
%destructor table_schema { delete $$; }
table_schema(A) ::=. { A = 0; }
table_schema(A) ::= LP sql_argument_list_nonempty(B) RP. { A = B; }

// CREATE statements
%type or_replace {int}
or_replace(A) ::=.                    { A = 0; }
or_replace(A) ::= OR REPLACE.         { A = 1; }

// CREATE PERFETTO FUNCTION
cmd ::= CREATE or_replace(R) PERFETTO FUNCTION ID(N) LP sql_argument_list(A) RP RETURNS return_type(T) AS select(E) pscantok(S). {
  std::unique_ptr<PerfettoSqlArgumentList> args_deleter(A);
  std::unique_ptr<PerfettoSqlFnReturnType> returns_deleter(T);

  PerfettoSqlParser::CreateFunction::Returns returns_res;
  returns_res.is_table = T->is_table;
  if (T->is_table) {
    returns_res.table_columns = std::move(T->table_columns);
  } else {
    returns_res.scalar_type = T->scalar_type;
  }

  state->current_statement = PerfettoSqlParser::CreateFunction{
      R != 0,
      FunctionPrototype{
          std::string(N.ptr, N.n),
          A ? std::move(A->inner)
               : std::vector<sql_argument::ArgumentDefinition>{},
      },
      std::move(returns_res),
      OnPerfettoSqlSubstr(state, E, S),
      "",
      std::nullopt,
  };
}

// CREATE PERFETTO FUNCTION with delegating implementation
cmd ::= CREATE or_replace(R) PERFETTO FUNCTION ID(N) LP sql_argument_list(A) RP RETURNS return_type(T) DELEGATES TO ID(I) pscantok. {
  std::unique_ptr<PerfettoSqlArgumentList> args_deleter(A);
  std::unique_ptr<PerfettoSqlFnReturnType> returns_deleter(T);

  if (I.n != 0) {
    PerfettoSqlParser::CreateFunction::Returns returns_res;
    returns_res.is_table = T->is_table;
    if (T->is_table) {
      returns_res.table_columns = std::move(T->table_columns);
    } else {
      returns_res.scalar_type = T->scalar_type;
    }

    state->current_statement = PerfettoSqlParser::CreateFunction{
        R != 0,
        FunctionPrototype{
            std::string(N.ptr, N.n),
            A ? std::move(A->inner)
                 : std::vector<sql_argument::ArgumentDefinition>{},
        },
        std::move(returns_res),
        SqlSource::FromTraceProcessorImplementation(""),
        "",
        std::string(I.ptr, I.n),
    };
  } else {
    OnPerfettoSqlError(state, "Target function name cannot be empty", I);
  }
}

%type return_type { struct PerfettoSqlFnReturnType* }
%destructor return_type { delete $$; }
return_type(Y) ::= ID(X). {
  auto parsed = OnPerfettoSqlParseType(X);
  if (!parsed) {
    Y = nullptr;
  } else {
    Y = new PerfettoSqlFnReturnType();
    Y->is_table = false;
    Y->scalar_type = *parsed;
  }
}
return_type(Y) ::= TABLE LP sql_argument_list_nonempty(A) RP. {
  Y = new PerfettoSqlFnReturnType();
  Y->is_table = true;
  Y->table_columns = std::move(A->inner);
  delete A;
}

table_impl(Y) ::=. {
  Y = (struct PerfettoSqlToken) {0, 0};
}
table_impl(Y) ::= USING ID(N). {
  Y = N;
}

// CREATE PERFETTO TABLE
cmd ::= CREATE or_replace(R) PERFETTO TABLE ID(N) table_impl(Y) table_schema(S) AS select(A) pscantok(Q). {
  std::unique_ptr<PerfettoSqlArgumentList> args_deleter(S);
  if (Y.n == 0 ||
      base::CaseInsensitiveEqual(std::string(Y.ptr, Y.n), "dataframe")) {
    state->current_statement = PerfettoSqlParser::CreateTable{
        R != 0,
        std::string(N.ptr, N.n),
        S ? std::move(S->inner)
             : std::vector<sql_argument::ArgumentDefinition>{},
        OnPerfettoSqlSubstrDefault(state, A, Q),
    };
  } else {
    OnPerfettoSqlError(state, "Invalid table implementation", Y);
  }
}

// CREATE PERFETTO VIEW
cmd ::= CREATE(C) or_replace(R) PERFETTO VIEW ID(N) table_schema(S) AS select(A) pscantok(Q). {
  std::unique_ptr<PerfettoSqlArgumentList> args_deleter(S);

  state->current_statement = PerfettoSqlParser::CreateView{
      R != 0,
      std::string(N.ptr, N.n),
      S ? std::move(S->inner)
           : std::vector<sql_argument::ArgumentDefinition>(),
      OnPerfettoSqlSubstrDefault(state, A, Q),
      OnPerfettoSqlRewriteView(state, C, N, A),
  };
}

// CREATE PERFETTO INDEX
cmd ::= CREATE or_replace(R) PERFETTO INDEX ID(N) ON ID(T) LP indexed_column_list(L) RP. {
  std::unique_ptr<PerfettoSqlIndexedColumnList> cols_deleter(L);

  state->current_statement = PerfettoSqlParser::CreateIndex{
      R != 0,
      std::string(N.ptr, N.n),
      std::string(T.ptr, T.n),
      std::move(L->cols),
  };
}

%type indexed_column_list { struct PerfettoSqlIndexedColumnList* }
%destructor indexed_column_list { delete $$; }
indexed_column_list(A) ::= indexed_column_list(B) COMMA ID(C). {
  B->cols.emplace_back(C.ptr, C.n);
  A = B;
}
indexed_column_list(A) ::= ID(B). {
  A = new PerfettoSqlIndexedColumnList();
  A->cols.emplace_back(B.ptr, B.n);
}

// CREATE PERFETTO MACRO
cmd ::= CREATE or_replace(R) PERFETTO MACRO ID(N) LP macro_argument_list(A) RP RETURNS ID(T) AS macro_body(S) pscantok(B). {
  std::unique_ptr<PerfettoSqlMacroArgumentList> args_deleter(A);

  state->current_statement = PerfettoSqlParser::CreateMacro{
      R != 0,
      OnPerfettoSqlExtractSource(state, N),
      A ? std::move(A->args)
           : std::vector<std::pair<SqlSource, SqlSource>>{},
      OnPerfettoSqlExtractSource(state, T),
      OnPerfettoSqlSubstrDefault(state, S, B),
  };
}
macro_body ::= ANY.
macro_body ::= macro_body ANY.

%type macro_argument_list_nonempty { struct PerfettoSqlMacroArgumentList* }
%destructor macro_argument_list_nonempty { delete $$; }
macro_argument_list_nonempty(A) ::= macro_argument_list_nonempty(D) COMMA ID(B) ID(C). {
  D->args.emplace_back(
      OnPerfettoSqlExtractSource(state, B),
      OnPerfettoSqlExtractSource(state, C));
  A = D;
}
macro_argument_list_nonempty(A) ::= ID(B) ID(C). {
  A = new PerfettoSqlMacroArgumentList();
  A->args.emplace_back(
      OnPerfettoSqlExtractSource(state, B),
      OnPerfettoSqlExtractSource(state, C));
}

%type macro_argument_list { struct PerfettoSqlMacroArgumentList* }
%destructor macro_argument_list { delete $$; }
macro_argument_list(A) ::=. { A = 0; }
macro_argument_list(A) ::= macro_argument_list_nonempty(B). { A = B; }

// INCLUDE statement
cmd ::= INCLUDE PERFETTO MODULE module_name(M). {
  state->current_statement =
      PerfettoSqlParser::Include{std::string(M.ptr, M.n)};
}
module_name(A) ::= ID|STAR|INTERSECT(B). {
  A = B;
}
module_name(A) ::= module_name(B) DOT ID|STAR|INTERSECT(C). {
  A = (struct PerfettoSqlToken) {B.ptr, static_cast<size_t>(C.ptr + C.n - B.ptr)};
}

// DROP statement
cmd ::= DROP PERFETTO INDEX ID(N) ON ID(T). {
  state->current_statement = PerfettoSqlParser::DropIndex{
      std::string(N.ptr, N.n),
      std::string(T.ptr, T.n),
  };
}
