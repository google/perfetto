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

// Perfetto dialect extension grammar rules — canonical source of truth.
//
// These rules extend syntaqlite's base SQLite grammar with PerfettoSQL
// syntax. Regenerate `syntaqlite_perfetto.{c,h}` via
// `tools/gen_syntaqlite_parser` after editing this file.

// Allow extension keywords to be used as regular identifiers.
%fallback ID PERFETTO FUNCTION MODULE RETURNS MACRO DELEGATES INCLUDE
          TREE ACCUMULATE UP DOWN INTERVAL INTERSECTION PER EXTEND.

// ---------- Helper nonterminals ----------

%type perfetto_or_replace {int}
perfetto_or_replace(A) ::= .            { A = 0; }
perfetto_or_replace(A) ::= OR REPLACE.  { A = 1; }

// Argument type: plain ID or JOINID(table.col) form.
%type perfetto_arg_type {SynqParseToken}
perfetto_arg_type(A) ::= ID(B). {
    synq_mark_as_type(pCtx, B);
    A = B;
}
perfetto_arg_type(A) ::= ID(B) LP ID DOT ID RP(E). {
    synq_mark_as_type(pCtx, B);
    A = (SynqParseToken){
        .z = B.z,
        .n = (uint32_t)(E.z + E.n - B.z),
        .type = B.type,
        .token_idx = B.token_idx,
        .offset = B.offset,
        .layer_id = B.layer_id,
    };
}

// Argument definition list for functions and table schemas.
%type perfetto_arg_def_list {uint32_t}
perfetto_arg_def_list(A) ::= . { A = SYNTAQLITE_NULL_NODE; }
perfetto_arg_def_list(A) ::= perfetto_arg_def_list_ne(X). { A = X; }

%type perfetto_arg_def_list_ne {uint32_t}
perfetto_arg_def_list_ne(A) ::= ID(N) perfetto_arg_type(T). {
    uint32_t arg = synq_parse_perfetto_arg_def(pCtx,
        synq_parse_ident_name(pCtx, synq_span(pCtx, N)), synq_span(pCtx, T),
        SYNTAQLITE_BOOL_FALSE);
    A = synq_parse_perfetto_arg_def_list(pCtx, SYNTAQLITE_NULL_NODE, arg);
}
perfetto_arg_def_list_ne(A) ::= perfetto_arg_def_list_ne(L) COMMA ID(N) perfetto_arg_type(T). {
    uint32_t arg = synq_parse_perfetto_arg_def(pCtx,
        synq_parse_ident_name(pCtx, synq_span(pCtx, N)), synq_span(pCtx, T),
        SYNTAQLITE_BOOL_FALSE);
    A = synq_parse_perfetto_arg_def_list(pCtx, L, arg);
}
// Variadic argument: name TYPE...
perfetto_arg_def_list_ne(A) ::= ID(N) perfetto_arg_type(T) DOT DOT DOT. {
    uint32_t arg = synq_parse_perfetto_arg_def(pCtx,
        synq_parse_ident_name(pCtx, synq_span(pCtx, N)), synq_span(pCtx, T),
        SYNTAQLITE_BOOL_TRUE);
    A = synq_parse_perfetto_arg_def_list(pCtx, SYNTAQLITE_NULL_NODE, arg);
}
perfetto_arg_def_list_ne(A) ::= perfetto_arg_def_list_ne(L) COMMA ID(N) perfetto_arg_type(T) DOT DOT DOT. {
    uint32_t arg = synq_parse_perfetto_arg_def(pCtx,
        synq_parse_ident_name(pCtx, synq_span(pCtx, N)), synq_span(pCtx, T),
        SYNTAQLITE_BOOL_TRUE);
    A = synq_parse_perfetto_arg_def_list(pCtx, L, arg);
}

// Table schema: optional parenthesized arg list.
%type perfetto_table_schema {uint32_t}
perfetto_table_schema(A) ::= . { A = SYNTAQLITE_NULL_NODE; }
perfetto_table_schema(A) ::= LP perfetto_arg_def_list_ne(L) RP. { A = L; }

// Table implementation: optional `USING <ident>` clause.
%type perfetto_table_impl {uint32_t}
perfetto_table_impl(A) ::= . { A = SYNTAQLITE_NULL_NODE; }
perfetto_table_impl(A) ::= USING ID(N). {
    A = synq_parse_perfetto_table_impl(pCtx, synq_span(pCtx, N));
}

// Return type for CREATE PERFETTO FUNCTION.
%type perfetto_return_type {uint32_t}
perfetto_return_type(A) ::= ID(T). {
    synq_mark_as_type(pCtx, T);
    A = synq_parse_perfetto_return_type(pCtx,
        SYNTAQLITE_PERFETTO_RETURN_KIND_SCALAR,
        synq_span(pCtx, T),
        SYNTAQLITE_NULL_NODE);
}
perfetto_return_type(A) ::= TABLE LP perfetto_arg_def_list_ne(L) RP. {
    A = synq_parse_perfetto_return_type(pCtx,
        SYNTAQLITE_PERFETTO_RETURN_KIND_TABLE,
        SYNQ_NO_SPAN,
        L);
}

// Indexed column list for CREATE PERFETTO INDEX.
%type perfetto_indexed_col_list {uint32_t}
perfetto_indexed_col_list(A) ::= ID(N). {
    uint32_t col = synq_parse_perfetto_indexed_column(pCtx, synq_span(pCtx, N));
    A = synq_parse_perfetto_indexed_column_list(pCtx, SYNTAQLITE_NULL_NODE, col);
}
perfetto_indexed_col_list(A) ::= perfetto_indexed_col_list(L) COMMA ID(N). {
    uint32_t col = synq_parse_perfetto_indexed_column(pCtx, synq_span(pCtx, N));
    A = synq_parse_perfetto_indexed_column_list(pCtx, L, col);
}

// Macro argument list.
%type perfetto_macro_arg_list {uint32_t}
perfetto_macro_arg_list(A) ::= . { A = SYNTAQLITE_NULL_NODE; }
perfetto_macro_arg_list(A) ::= perfetto_macro_arg_list_ne(X). { A = X; }

%type perfetto_macro_arg_list_ne {uint32_t}
perfetto_macro_arg_list_ne(A) ::= ID(N) ID(T). {
    synq_mark_as_type(pCtx, T);
    uint32_t arg = synq_parse_perfetto_macro_arg(pCtx,
        synq_span(pCtx, N), synq_span(pCtx, T));
    A = synq_parse_perfetto_macro_arg_list(pCtx, SYNTAQLITE_NULL_NODE, arg);
}
perfetto_macro_arg_list_ne(A) ::= perfetto_macro_arg_list_ne(L) COMMA ID(N) ID(T). {
    synq_mark_as_type(pCtx, T);
    uint32_t arg = synq_parse_perfetto_macro_arg(pCtx,
        synq_span(pCtx, N), synq_span(pCtx, T));
    A = synq_parse_perfetto_macro_arg_list(pCtx, L, arg);
}

// Module name: dotted path like foo.bar.baz
%type perfetto_module_name {SynqParseToken}
perfetto_module_name(A) ::= ID|STAR|INTERSECT(B). { A = B; }
perfetto_module_name(A) ::= perfetto_module_name(B) DOT ID|STAR|INTERSECT(C). {
    A = (SynqParseToken){
        .z = B.z,
        .n = (uint32_t)(C.z + C.n - B.z),
        .type = B.type,
        .token_idx = B.token_idx,
        .offset = B.offset,
        .layer_id = B.layer_id,
    };
}

// Empty marker rules used to bracket a `select` subrule so the parent
// production can compute an exact source span covering the authored body.
// select_body_start reads cur_shift_start — the start of the token Lemon is
// currently processing, excluding leading whitespace before the body.
// select_body_end reads last_shifted_end — the end of the last terminal of
// select, set by record_and_feed after Lemon finishes processing it.
%type select_body_start {uint32_t}
select_body_start(A) ::= . { A = pCtx->cur_shift_start; }

%type select_body_end {uint32_t}
select_body_end(A) ::= . { A = pCtx->last_shifted_end; }

// ---------- Pipelines ----------

// `|>` is not a token the SQLite tokenizer knows, so a pipe is `|` directly
// followed by `>`. No valid SQL expression contains those two in a row, but
// an expression can end in `|`, so nothing that ends in a bare expression may
// precede a pipe: see perfetto_pipe_source.
%type perfetto_pipe {int}
perfetto_pipe(A) ::= BITOR(B) GT(G). {
    if (B.layer_id != G.layer_id || B.offset + B.n != G.offset) {
        pCtx->error = 1;
    }
    A = 0;
}

// What a pipeline may start from: a table or a parenthesised subquery. A join
// is not allowed here since its ON clause is an expression, whose trailing `|`
// would be ambiguous with the pipe; wrap it in a subquery instead.
%type perfetto_pipe_source {uint32_t}
perfetto_pipe_source(A) ::= nm(N) dbnm(D) as(Z). {
    SyntaqliteTextSpan table_name;
    SyntaqliteTextSpan schema;
    if (D.z != NULL) {
        table_name = synq_span_dequote(pCtx, D);
        schema = synq_span_dequote(pCtx, N);
    } else {
        table_name = synq_span_dequote(pCtx, N);
        schema = SYNQ_NO_SPAN;
    }
    A = synq_parse_perfetto_pipe_source(pCtx, table_name, schema,
        SYNTAQLITE_NULL_NODE, Z.name,
        Z.has_as ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE);
}
perfetto_pipe_source(A) ::= LP select(S) RP as(Z). {
    A = synq_parse_perfetto_pipe_source(pCtx, SYNQ_NO_SPAN, SYNQ_NO_SPAN,
        S, Z.name, Z.has_as ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE);
}
// A pipeline read by a pipeline is compiled into the same plan, so it is left
// as it is written rather than expanded into SQL.
perfetto_pipe_source(A) ::= LP perfetto_pipeline(P) RP as(Z). {
    A = synq_parse_perfetto_pipe_source(pCtx, SYNQ_NO_SPAN, SYNQ_NO_SPAN,
        P, Z.name, Z.has_as ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE);
}

%type perfetto_tree_direction {int}
perfetto_tree_direction(A) ::= UP.   { A = SYNTAQLITE_PERFETTO_TREE_DIRECTION_UP; }
perfetto_tree_direction(A) ::= DOWN. { A = SYNTAQLITE_PERFETTO_TREE_DIRECTION_DOWN; }

%type perfetto_tree_aggregate {uint32_t}
perfetto_tree_aggregate(A) ::= expr(E) AS nm(N). {
    A = synq_parse_perfetto_tree_aggregate(pCtx, E,
        synq_span_dequote(pCtx, N));
}

%type perfetto_tree_aggregate_list {uint32_t}
perfetto_tree_aggregate_list(A) ::= perfetto_tree_aggregate(X). {
    A = synq_parse_perfetto_tree_aggregate_list(pCtx, SYNTAQLITE_NULL_NODE, X);
}
perfetto_tree_aggregate_list(A) ::= perfetto_tree_aggregate_list(L) COMMA
                                    perfetto_tree_aggregate(X). {
    A = synq_parse_perfetto_tree_aggregate_list(pCtx, L, X);
}

// A column, optionally qualified: `column` or `alias.column`.
%type perfetto_pipe_column {uint32_t}
perfetto_pipe_column(A) ::= nm(N). {
    A = synq_parse_perfetto_pipe_column(pCtx, SYNQ_NO_SPAN,
        synq_span_dequote(pCtx, N), SYNQ_NO_SPAN);
}
perfetto_pipe_column(A) ::= nm(Q) DOT nm(N). {
    A = synq_parse_perfetto_pipe_column(pCtx, synq_span_dequote(pCtx, Q),
        synq_span_dequote(pCtx, N), SYNQ_NO_SPAN);
}

// A column given a name: `column [AS] name`, optionally qualified.
%type perfetto_pipe_named_column {uint32_t}
perfetto_pipe_named_column(A) ::= nm(N) AS nm(R). {
    A = synq_parse_perfetto_pipe_column(pCtx, SYNQ_NO_SPAN,
        synq_span_dequote(pCtx, N), synq_span_dequote(pCtx, R));
}
perfetto_pipe_named_column(A) ::= nm(N) nm(R). {
    A = synq_parse_perfetto_pipe_column(pCtx, SYNQ_NO_SPAN,
        synq_span_dequote(pCtx, N), synq_span_dequote(pCtx, R));
}
perfetto_pipe_named_column(A) ::= nm(Q) DOT nm(N) AS nm(R). {
    A = synq_parse_perfetto_pipe_column(pCtx, synq_span_dequote(pCtx, Q),
        synq_span_dequote(pCtx, N), synq_span_dequote(pCtx, R));
}
perfetto_pipe_named_column(A) ::= nm(Q) DOT nm(N) nm(R). {
    A = synq_parse_perfetto_pipe_column(pCtx, synq_span_dequote(pCtx, Q),
        synq_span_dequote(pCtx, N), synq_span_dequote(pCtx, R));
}

// Bare column names, as listed by DROP and EXCEPT.
%type perfetto_pipe_name_list {uint32_t}
perfetto_pipe_name_list(A) ::= nm(N). {
    A = synq_parse_perfetto_pipe_name_list(pCtx, SYNTAQLITE_NULL_NODE,
        synq_parse_perfetto_pipe_name(pCtx, synq_span_dequote(pCtx, N)));
}
perfetto_pipe_name_list(A) ::= perfetto_pipe_name_list(L) COMMA nm(N). {
    A = synq_parse_perfetto_pipe_name_list(pCtx, L,
        synq_parse_perfetto_pipe_name(pCtx, synq_span_dequote(pCtx, N)));
}

// A star's REPLACE list: `column AS name, ...`, where AS is required.
%type perfetto_pipe_replace_item {uint32_t}
perfetto_pipe_replace_item(A) ::= nm(N) AS nm(R). {
    A = synq_parse_perfetto_pipe_column(pCtx, SYNQ_NO_SPAN,
        synq_span_dequote(pCtx, N), synq_span_dequote(pCtx, R));
}
perfetto_pipe_replace_item(A) ::= nm(Q) DOT nm(N) AS nm(R). {
    A = synq_parse_perfetto_pipe_column(pCtx, synq_span_dequote(pCtx, Q),
        synq_span_dequote(pCtx, N), synq_span_dequote(pCtx, R));
}

%type perfetto_pipe_replace_list {uint32_t}
perfetto_pipe_replace_list(A) ::= perfetto_pipe_replace_item(X). {
    A = synq_parse_perfetto_pipe_column_list(pCtx, SYNTAQLITE_NULL_NODE, X);
}
perfetto_pipe_replace_list(A) ::= perfetto_pipe_replace_list(L) COMMA
                                  perfetto_pipe_replace_item(X). {
    A = synq_parse_perfetto_pipe_column_list(pCtx, L, X);
}

%type perfetto_pipe_except {uint32_t}
perfetto_pipe_except(A) ::= . { A = SYNTAQLITE_NULL_NODE; }
perfetto_pipe_except(A) ::= EXCEPT LP perfetto_pipe_name_list(L) RP. {
    A = L;
}

%type perfetto_pipe_replace {uint32_t}
perfetto_pipe_replace(A) ::= . { A = SYNTAQLITE_NULL_NODE; }
perfetto_pipe_replace(A) ::= REPLACE LP perfetto_pipe_replace_list(L) RP. {
    A = L;
}

// One item of a SELECT or EXTEND stage: a column, optionally qualified and
// named, or a star.
%type perfetto_pipe_select_item {uint32_t}
perfetto_pipe_select_item(A) ::= perfetto_pipe_column(X). { A = X; }
perfetto_pipe_select_item(A) ::= perfetto_pipe_named_column(X). { A = X; }
perfetto_pipe_select_item(A) ::= STAR perfetto_pipe_except(E)
                                 perfetto_pipe_replace(R). {
    A = synq_parse_perfetto_pipe_star(pCtx, SYNQ_NO_SPAN, E, R);
}
perfetto_pipe_select_item(A) ::= nm(Q) DOT STAR perfetto_pipe_except(E)
                                 perfetto_pipe_replace(R). {
    A = synq_parse_perfetto_pipe_star(pCtx, synq_span_dequote(pCtx, Q), E, R);
}

%type perfetto_pipe_select_list {uint32_t}
perfetto_pipe_select_list(A) ::= perfetto_pipe_select_item(X). {
    A = synq_parse_perfetto_pipe_select_item_list(pCtx, SYNTAQLITE_NULL_NODE,
        X);
}
perfetto_pipe_select_list(A) ::= perfetto_pipe_select_list(L) COMMA
                                 perfetto_pipe_select_item(X). {
    A = synq_parse_perfetto_pipe_select_item_list(pCtx, L, X);
}

// RENAME's list: `old [AS] new, ...`, with bare names only.
%type perfetto_pipe_rename_item {uint32_t}
perfetto_pipe_rename_item(A) ::= nm(N) AS nm(R). {
    A = synq_parse_perfetto_pipe_column(pCtx, SYNQ_NO_SPAN,
        synq_span_dequote(pCtx, N), synq_span_dequote(pCtx, R));
}
perfetto_pipe_rename_item(A) ::= nm(N) nm(R). {
    A = synq_parse_perfetto_pipe_column(pCtx, SYNQ_NO_SPAN,
        synq_span_dequote(pCtx, N), synq_span_dequote(pCtx, R));
}

%type perfetto_pipe_rename_list {uint32_t}
perfetto_pipe_rename_list(A) ::= perfetto_pipe_rename_item(X). {
    A = synq_parse_perfetto_pipe_column_list(pCtx, SYNTAQLITE_NULL_NODE, X);
}
perfetto_pipe_rename_list(A) ::= perfetto_pipe_rename_list(L) COMMA
                                 perfetto_pipe_rename_item(X). {
    A = synq_parse_perfetto_pipe_column_list(pCtx, L, X);
}

// SET's list: `name = column, ...`.
%type perfetto_pipe_set_item {uint32_t}
perfetto_pipe_set_item(A) ::= nm(N) EQ perfetto_pipe_column(V). {
    A = synq_parse_perfetto_pipe_set_item(pCtx, synq_span_dequote(pCtx, N), V);
}

%type perfetto_pipe_set_list {uint32_t}
perfetto_pipe_set_list(A) ::= perfetto_pipe_set_item(X). {
    A = synq_parse_perfetto_pipe_set_item_list(pCtx, SYNTAQLITE_NULL_NODE, X);
}
perfetto_pipe_set_list(A) ::= perfetto_pipe_set_list(L) COMMA
                              perfetto_pipe_set_item(X). {
    A = synq_parse_perfetto_pipe_set_item_list(pCtx, L, X);
}

%type perfetto_pipe_stage {uint32_t}
perfetto_pipe_stage(A) ::= SELECT perfetto_pipe_select_list(L). {
    A = synq_parse_perfetto_pipe_select(pCtx, L);
}
perfetto_pipe_stage(A) ::= EXTEND perfetto_pipe_select_list(L). {
    A = synq_parse_perfetto_pipe_extend(pCtx, L);
}
perfetto_pipe_stage(A) ::= DROP perfetto_pipe_name_list(L). {
    A = synq_parse_perfetto_pipe_drop(pCtx, L);
}
perfetto_pipe_stage(A) ::= RENAME perfetto_pipe_rename_list(L). {
    A = synq_parse_perfetto_pipe_rename(pCtx, L);
}
perfetto_pipe_stage(A) ::= SET perfetto_pipe_set_list(L). {
    A = synq_parse_perfetto_pipe_set(pCtx, L);
}
perfetto_pipe_stage(A) ::= AS nm(N). {
    A = synq_parse_perfetto_pipe_as(pCtx, synq_span_dequote(pCtx, N));
}
perfetto_pipe_stage(A) ::= TREE ACCUMULATE perfetto_tree_direction(D)
                           perfetto_tree_aggregate_list(L). {
    A = synq_parse_perfetto_tree_accumulate(pCtx,
        (SyntaqlitePerfettoTreeDirection)D, L);
}

%type perfetto_pipe_stage_list {uint32_t}
perfetto_pipe_stage_list(A) ::= . { A = SYNTAQLITE_NULL_NODE; }
perfetto_pipe_stage_list(A) ::= perfetto_pipe_stage_list(L) perfetto_pipe
                                perfetto_pipe_stage(S). {
    A = synq_parse_perfetto_pipe_stage_list(pCtx, L, S);
}

// The operands of an interval source, each a relation with an alias.
%type perfetto_pipe_source_list {uint32_t}
perfetto_pipe_source_list(A) ::= perfetto_pipe_source(X). {
    A = synq_parse_perfetto_pipe_source_list(pCtx, SYNTAQLITE_NULL_NODE, X);
}
perfetto_pipe_source_list(A) ::= perfetto_pipe_source_list(L) COMMA
                                 perfetto_pipe_source(X). {
    A = synq_parse_perfetto_pipe_source_list(pCtx, L, X);
}

%type perfetto_per_col_list {uint32_t}
perfetto_per_col_list(A) ::= nm(N). {
    uint32_t col = synq_parse_perfetto_per_column(pCtx,
        synq_span_dequote(pCtx, N));
    A = synq_parse_perfetto_per_column_list(pCtx, SYNTAQLITE_NULL_NODE, col);
}
perfetto_per_col_list(A) ::= perfetto_per_col_list(L) COMMA nm(N). {
    uint32_t col = synq_parse_perfetto_per_column(pCtx,
        synq_span_dequote(pCtx, N));
    A = synq_parse_perfetto_per_column_list(pCtx, L, col);
}

%type perfetto_per {uint32_t}
perfetto_per(A) ::= . { A = SYNTAQLITE_NULL_NODE; }
perfetto_per(A) ::= PER perfetto_per_col_list(L). { A = L; }

%type perfetto_pipeline {uint32_t}
perfetto_pipeline(A) ::= FROM perfetto_pipe_source(F)
                         perfetto_pipe_stage_list(S). {
    A = synq_parse_perfetto_pipeline(pCtx, F, SYNTAQLITE_NULL_NODE, S);
}
perfetto_pipeline(A) ::= INTERVAL INTERSECTION OF LP
                         perfetto_pipe_source_list(L) RP perfetto_per(P)
                         perfetto_pipe_stage_list(S). {
    uint32_t src = synq_parse_perfetto_interval_intersection(pCtx, L, P);
    A = synq_parse_perfetto_pipeline(pCtx, SYNTAQLITE_NULL_NODE, src, S);
}

cmd(A) ::= perfetto_pipeline(P). { A = P; }

// A pipeline in parentheses reads like any other subquery, in a FROM clause or
// as a CTE. Once parsed, it is handed to the engine, which replaces it with
// SQL reading it.
%type perfetto_subquery_pipeline {uint32_t}
perfetto_subquery_pipeline(A) ::= perfetto_pipeline(P). {
    A = P;
    synq_parser_expand_node(pCtx, P, "pipeline", 8);
}

seltablist(A) ::= stl_prefix(A) LP perfetto_subquery_pipeline(P) RP as(Z)
                  on_using(N). {
    pCtx->saw_subquery = 1;
    uint32_t sub = synq_parse_subquery_table_source(
        pCtx, P, Z.name,
        Z.has_as ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE);
    if (A == SYNTAQLITE_NULL_NODE) {
        synq_reject_dangling_on_using(pCtx, N);
        A = sub;
    } else {
        SyntaqliteNode *pfx = AST_NODE(&pCtx->ast, A);
        A = synq_parse_join_clause(pCtx,
            pfx->join_prefix.join_type,
            pfx->join_prefix.modifiers,
            pfx->join_prefix.source,
            sub, N.on_expr, N.using_cols);
    }
}
wqitem(A) ::= withnm(X) eidlist_opt(Y) wqas(M) LP perfetto_subquery_pipeline(P)
              RP. {
    A = synq_parse_cte_definition(pCtx, synq_span_dequote(pCtx, X),
                                  (SyntaqliteMaterialized)M, Y, P);
}

// ---------- PERFETTO PRAGMA ----------

// A setting of the engine, rather than of SQLite. Any expression parses;
// the engine takes the ones it can read.
cmd(A) ::= PERFETTO PRAGMA nm(N) EQ expr(V). {
    A = synq_parse_perfetto_pragma_stmt(pCtx, synq_span_dequote(pCtx, N), V);
}

// ---------- CREATE PERFETTO TABLE ----------

cmd(A) ::= CREATE perfetto_or_replace(R) PERFETTO TABLE nm(N)
           perfetto_table_impl(I) perfetto_table_schema(S)
           AS select_body_start(BS) select(E) select_body_end(BE). {
    SyntaqliteTextSpan select_span = {
        .offset = BS,
        .length = BE - BS,
    };
    A = synq_parse_create_perfetto_table_stmt(pCtx,
        synq_span(pCtx, N),
        R ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE,
        I, S, E, select_span, SYNTAQLITE_NULL_NODE);
}

cmd(A) ::= CREATE perfetto_or_replace(R) PERFETTO TABLE nm(N)
           perfetto_table_impl(I) perfetto_table_schema(S)
           AS perfetto_pipeline(P). {
    A = synq_parse_create_perfetto_table_stmt(pCtx,
        synq_span(pCtx, N),
        R ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE,
        I, S, SYNTAQLITE_NULL_NODE, SYNQ_NO_SPAN, P);
}

// ---------- CREATE PERFETTO VIEW ----------

cmd(A) ::= CREATE perfetto_or_replace(R) PERFETTO VIEW nm(N)
           perfetto_table_schema(S) AS select_body_start(BS) select(E) select_body_end(BE). {
    SyntaqliteTextSpan select_span = {
        .offset = BS,
        .length = BE - BS,
    };
    A = synq_parse_create_perfetto_view_stmt(pCtx,
        synq_span(pCtx, N),
        R ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE,
        S, E, select_span);
}

// ---------- CREATE PERFETTO FUNCTION ----------

cmd(A) ::= CREATE perfetto_or_replace(R) PERFETTO FUNCTION nm(N) LP
           perfetto_arg_def_list(ARGS) RP RETURNS perfetto_return_type(RT)
           AS select_body_start(BS) select(E) select_body_end(BE). {
    SyntaqliteTextSpan select_span = {
        .offset = BS,
        .length = BE - BS,
    };
    A = synq_parse_create_perfetto_function_stmt(pCtx,
        synq_span(pCtx, N),
        R ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE,
        ARGS, RT, E, select_span);
}

// ---------- CREATE PERFETTO FUNCTION (delegating) ----------

cmd(A) ::= CREATE perfetto_or_replace(R) PERFETTO FUNCTION nm(N) LP perfetto_arg_def_list(ARGS) RP RETURNS perfetto_return_type(RT) DELEGATES TO ID(I). {
    A = synq_parse_create_perfetto_delegating_function_stmt(pCtx,
        synq_span(pCtx, N),
        R ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE,
        ARGS, RT, synq_span(pCtx, I));
}

// ---------- CREATE PERFETTO INDEX ----------

cmd(A) ::= CREATE perfetto_or_replace(R) PERFETTO INDEX nm(N) ON nm(T) LP perfetto_indexed_col_list(L) RP. {
    A = synq_parse_create_perfetto_index_stmt(pCtx,
        synq_span(pCtx, N),
        R ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE,
        synq_span(pCtx, T),
        L);
}

// Empty marker rule that fires as a default reduction right after the parser
// shifts the return-type ID and BEFORE it shifts AS.  Lemon reduces the empty
// rule eagerly to satisfy the LALR table, which sets the "in macro definition
// body" flag in the parser context.  The tokenizer then skips nested macro
// expansion while reading the body so the body is captured verbatim.  The
// flag is cleared in the main cmd action when CREATE PERFETTO MACRO reduces.
%type before_macro_body {int}
before_macro_body(A) ::= . {
    pCtx->in_macro_def_body++;
    A = 0;
}

// Macro body: consumes arbitrary tokens via the %wildcard ANY mechanism.
%type perfetto_macro_body {SynqParseToken}
perfetto_macro_body(A) ::= ANY(B). { A = B; }
perfetto_macro_body(A) ::= perfetto_macro_body(B) ANY(C). {
    A = (SynqParseToken){
        .z = B.z,
        .n = (uint32_t)(C.z + C.n - B.z),
        .type = B.type,
        .token_idx = B.token_idx,
        .offset = B.offset,
        .layer_id = B.layer_id,
    };
}

// ---------- CREATE PERFETTO MACRO ----------

cmd(A) ::= CREATE perfetto_or_replace(R) PERFETTO MACRO nm(N) LP perfetto_macro_arg_list(ARGS) RP RETURNS ID(T) before_macro_body AS perfetto_macro_body(BODY). {
    synq_mark_as_type(pCtx, T);
    if (pCtx->in_macro_def_body > 0) pCtx->in_macro_def_body--;
    A = synq_parse_create_perfetto_macro_stmt(pCtx,
        synq_span(pCtx, N),
        R ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE,
        synq_span(pCtx, T),
        synq_span(pCtx, BODY),
        ARGS);
}


// ---------- INCLUDE PERFETTO MODULE ----------

cmd(A) ::= INCLUDE PERFETTO MODULE perfetto_module_name(M). {
    A = synq_parse_include_perfetto_module_stmt(pCtx,
        synq_span(pCtx, M));
}

// ---------- DROP PERFETTO INDEX ----------

cmd(A) ::= DROP PERFETTO INDEX nm(N) ON nm(T). {
    A = synq_parse_drop_perfetto_index_stmt(pCtx,
        synq_span(pCtx, N),
        synq_span(pCtx, T));
}
