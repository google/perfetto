// Copyright (C) 2025 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// PerfettoSQL Formatter — walks the Lezer AST and emits formatted SQL.

import {TreeCursor} from '@lezer/common';
import {parser} from './perfetto_sql.grammar';

const INDENT = '  ';

const KEYWORDS = new Set([
  'select', 'from', 'where', 'join', 'left', 'right', 'inner', 'outer',
  'cross', 'natural', 'on', 'using', 'group', 'by', 'order', 'having',
  'limit', 'offset', 'as', 'and', 'or', 'not', 'in', 'is', 'null',
  'case', 'when', 'then', 'else', 'end', 'cast', 'exists', 'between',
  'like', 'glob', 'union', 'all', 'intersect', 'except', 'distinct',
  'asc', 'desc', 'with', 'recursive', 'materialized', 'create',
  'perfetto', 'table', 'view', 'function', 'macro', 'index', 'virtual',
  'include', 'module', 'returns', 'delegates', 'to', 'over', 'partition',
  'true', 'false',
]);

interface FormatCtx {
  readonly sql: string;
  readonly depth: number;
}

export function formatSQL(sql: string): string {
  const tree = parser.parse(sql);
  const cursor = tree.cursor();
  const ctx: FormatCtx = {sql, depth: 0};
  const result = formatNode(cursor, ctx);
  return result.trim() + '\n';
}

function src(cursor: TreeCursor, ctx: FormatCtx): string {
  return ctx.sql.slice(cursor.from, cursor.to);
}

function ind(ctx: FormatCtx): string {
  return INDENT.repeat(ctx.depth);
}

function kw(text: string): string {
  if (KEYWORDS.has(text.toLowerCase())) {
    return text.toUpperCase();
  }
  return text;
}

function uppercaseKeywords(text: string): string {
  return text
    .replace(/\s+/g, ' ')
    .trim()
    .replace(/"[^"]*"|'[^']*'|\b([a-zA-Z_]+)\b/g, (match, word) => {
      if (!word) return match; // Quoted string — return as-is
      if (KEYWORDS.has(word.toLowerCase())) {
        return word.toUpperCase();
      }
      return word;
    });
}

function formatNode(cursor: TreeCursor, ctx: FormatCtx): string {
  switch (cursor.name) {
    case 'Program': return fmtProgram(cursor, ctx);
    case 'Statement': return fmtStatement(cursor, ctx);
    case 'SelectStatement': return fmtSelectStatement(cursor, ctx);
    case 'SelectBody': return fmtSelectBody(cursor, ctx);
    case 'SelectClauses': return fmtSelectClauses(cursor, ctx);
    case 'SelectColumns': return fmtSelectColumns(cursor, ctx);
    case 'SelectColumn': return fmtSelectColumn(cursor, ctx);
    case 'SetOperation': return fmtSetOperation(cursor, ctx);
    case 'FromClause': return fmtFromClause(cursor, ctx);
    case 'TableRef': return fmtTableRef(cursor, ctx);
    case 'TableSource': return fmtTableSource(cursor, ctx);
    case 'IdentifierPath': return src(cursor, ctx);
    case 'JoinClause': return fmtJoinClause(cursor, ctx);
    case 'JoinType': return fmtJoinType(cursor, ctx);
    case 'JoinConstraint': return fmtJoinConstraint(cursor, ctx);
    case 'WhereClause': return fmtWhereClause(cursor, ctx);
    case 'GroupByClause': return fmtGroupByClause(cursor, ctx);
    case 'HavingClause': return fmtHavingClause(cursor, ctx);
    case 'OrderByClause': return fmtOrderByClause(cursor, ctx);
    case 'OrderingTerm': return fmtOrderingTerm(cursor, ctx);
    case 'LimitClause': return fmtLimitClause(cursor, ctx);
    case 'WithStatement': return fmtWithStatement(cursor, ctx);
    case 'WithClause': return fmtWithClause(cursor, ctx);
    case 'CommonTableExpression': return fmtCTE(cursor, ctx);
    case 'QueryBody': return fmtQueryBody(cursor, ctx);
    case 'CreatePerfettoTableStatement': return fmtCreateTable(cursor, ctx);
    case 'CreatePerfettoViewStatement': return fmtCreateView(cursor, ctx);
    case 'CreatePerfettoFunctionStatement': return fmtCreateFunction(cursor, ctx);
    case 'CreatePerfettoMacroStatement': return fmtCreateMacro(cursor, ctx);
    case 'CreatePerfettoIndexStatement': return fmtCreateIndex(cursor, ctx);
    case 'CreateVirtualTableStatement': return fmtCreateVirtualTable(cursor, ctx);
    case 'IncludeModuleStatement': return fmtIncludeModule(cursor, ctx);
    case 'ColumnDefList': return fmtColumnDefList(cursor, ctx);
    case 'ColumnDef': return fmtColumnDef(cursor, ctx);
    case 'ColumnType': return fmtColumnType(cursor, ctx);
    case 'ColumnNameList': return src(cursor, ctx).replace(/\s*,\s*/g, ', ');
    case 'FunctionParamList': return fmtFunctionParamList(cursor, ctx);
    case 'FunctionParam': return fmtFunctionParam(cursor, ctx);
    case 'FunctionReturnType': return kw(src(cursor, ctx));
    case 'MacroParamList': return fmtMacroParamList(cursor, ctx);
    case 'MacroParam': return src(cursor, ctx);
    case 'VirtualTableArgList': return fmtVirtualTableArgList(cursor, ctx);
    case 'VirtualTableArg': return kw(src(cursor, ctx));
    case 'ModulePath': return src(cursor, ctx);
    case 'Expression': return uppercaseKeywords(src(cursor, ctx));
    case 'ExpressionList': return fmtExpressionList(cursor, ctx);
    case 'ArgList': return fmtExpressionList(cursor, ctx);
    case 'FunctionCall':
    case 'MacroInvocation':
    case 'WindowOver':
    case 'WindowBody':
    case 'CaseExpr':
    case 'CastExpr':
    case 'ExistsExpr':
      return uppercaseKeywords(src(cursor, ctx));
    case 'ParenExpr': return fmtParenExpr(cursor, ctx);
    default: return kw(src(cursor, ctx));
  }
}

// ---------------------------------------------------------------------------
// Program / Statement
// ---------------------------------------------------------------------------

function fmtProgram(cursor: TreeCursor, ctx: FormatCtx): string {
  const parts: string[] = [];
  if (cursor.firstChild()) {
    do { parts.push(formatNode(cursor, ctx)); } while (cursor.nextSibling());
    cursor.parent();
  }
  return parts.join('\n');
}

function fmtStatement(cursor: TreeCursor, ctx: FormatCtx): string {
  if (!cursor.firstChild()) return '';
  let result = formatNode(cursor, ctx);
  while (cursor.nextSibling()) {
    if (cursor.name === 'Semi') result += ';';
  }
  cursor.parent();
  return result;
}

// ---------------------------------------------------------------------------
// SELECT
// ---------------------------------------------------------------------------

function fmtSelectStatement(cursor: TreeCursor, ctx: FormatCtx): string {
  const parts: string[] = [];
  if (cursor.firstChild()) {
    do { parts.push(formatNode(cursor, ctx)); } while (cursor.nextSibling());
    cursor.parent();
  }
  return parts.join('\n');
}

function fmtSelectBody(cursor: TreeCursor, ctx: FormatCtx): string {
  if (!cursor.firstChild()) return '';
  let selectKw = '';
  let distinct = false;
  let columns = '';
  let clauses = '';
  do {
    switch (cursor.name) {
      case 'SELECT': selectKw = 'SELECT'; break;
      case 'DISTINCT': distinct = true; break;
      case 'SelectColumns': columns = fmtSelectColumns(cursor, ctx); break;
      case 'SelectClauses': clauses = fmtSelectClauses(cursor, ctx); break;
      default: break;
    }
  } while (cursor.nextSibling());
  cursor.parent();
  let result = ind(ctx) + selectKw + (distinct ? ' DISTINCT' : '') + '\n';
  result += columns.trimEnd();
  result += clauses;
  return result;
}

function fmtSelectColumns(cursor: TreeCursor, ctx: FormatCtx): string {
  const cols: string[] = [];
  if (cursor.firstChild()) {
    do {
      if (cursor.name === 'SelectColumn') {
        cols.push(fmtSelectColumn(cursor, ctx));
      }
    } while (cursor.nextSibling());
    cursor.parent();
  }
  const deeper = ctx.depth + 1;
  return cols.map((c, i) =>
    INDENT.repeat(deeper) + c + (i < cols.length - 1 ? ',' : '')
  ).join('\n') + '\n';
}

function fmtSelectColumn(cursor: TreeCursor, ctx: FormatCtx): string {
  const nodeEnd = cursor.to;
  if (!cursor.firstChild()) return '';
  let expr = '';
  let asEnd = -1;
  do {
    if (cursor.name === 'Star') expr = '*';
    else if (cursor.name === 'Expression') expr = uppercaseKeywords(src(cursor, ctx));
    else if (cursor.name === 'AS') asEnd = cursor.to;
  } while (cursor.nextSibling());
  cursor.parent();
  if (asEnd >= 0) {
    const alias = ctx.sql.slice(asEnd, nodeEnd).trim();
    return expr + ' AS ' + alias;
  }
  return expr;
}

function fmtSelectClauses(cursor: TreeCursor, ctx: FormatCtx): string {
  const parts: string[] = [];
  if (cursor.firstChild()) {
    do { parts.push(formatNode(cursor, ctx)); } while (cursor.nextSibling());
    cursor.parent();
  }
  if (parts.length > 0) return '\n' + parts.join('\n');
  return '';
}

function fmtSetOperation(cursor: TreeCursor, ctx: FormatCtx): string {
  if (!cursor.firstChild()) return '';
  const kwParts: string[] = [];
  let body = '';
  do {
    if (cursor.name === 'SelectBody') body = fmtSelectBody(cursor, ctx);
    else if (cursor.name !== 'Comma' && cursor.name !== 'Semi') {
      kwParts.push(kw(src(cursor, ctx)));
    }
  } while (cursor.nextSibling());
  cursor.parent();
  return ind(ctx) + kwParts.join(' ') + '\n' + body;
}

// ---------------------------------------------------------------------------
// FROM / JOIN
// ---------------------------------------------------------------------------

function fmtFromClause(cursor: TreeCursor, ctx: FormatCtx): string {
  if (!cursor.firstChild()) return '';
  let result = ind(ctx) + 'FROM';
  do {
    if (cursor.name === 'FROM') continue;
    if (cursor.name === 'TableRef') result += ' ' + fmtTableRef(cursor, ctx);
    else if (cursor.name === 'JoinClause') result += '\n' + fmtJoinClause(cursor, ctx);
  } while (cursor.nextSibling());
  cursor.parent();
  return result;
}

function fmtTableRef(cursor: TreeCursor, ctx: FormatCtx): string {
  const nodeEnd = cursor.to;
  if (!cursor.firstChild()) return '';
  let source = '';
  let asEnd = -1;
  do {
    if (cursor.name === 'TableSource') source = fmtTableSource(cursor, ctx);
    else if (cursor.name === 'AS') asEnd = cursor.to;
  } while (cursor.nextSibling());
  cursor.parent();
  if (asEnd >= 0) {
    return source + ' AS ' + ctx.sql.slice(asEnd, nodeEnd).trim();
  }
  return source;
}

function fmtTableSource(cursor: TreeCursor, ctx: FormatCtx): string {
  if (!cursor.firstChild()) return '';
  const first = cursor.name;
  if (first === 'ParenL') {
    cursor.nextSibling();
    const nextName: string = cursor.name;
    if (nextName === 'QueryBody') {
      // Check if the inner content actually starts with SELECT or WITH.
      // Lezer error recovery can produce a QueryBody node for
      // parenthesized table names like (slice) — detect that case and
      // preserve the raw text instead of formatting as a subquery.
      const innerText = src(cursor, ctx).trimStart().toLowerCase();
      if (innerText.startsWith('select') || innerText.startsWith('with')) {
        const inner = fmtQueryBody(cursor, {...ctx, depth: ctx.depth + 1});
        cursor.parent();
        return '(\n' + inner + ind(ctx) + ')';
      }
    }
    cursor.parent();
    return uppercaseKeywords(src(cursor, ctx));
  }
  if (first === 'MacroVariable') {
    const text = src(cursor, ctx);
    cursor.parent();
    return text;
  }
  let path = src(cursor, ctx); // IdentifierPath
  let isMacro = false;
  let args = '';
  let hasParen = false;
  while (cursor.nextSibling()) {
    if (cursor.name === 'BangL') { isMacro = true; hasParen = true; }
    else if (cursor.name === 'ParenL') hasParen = true;
    else if (cursor.name === 'ArgList') args = fmtExpressionList(cursor, ctx);
  }
  cursor.parent();
  if (hasParen) return path + (isMacro ? '!(' : '(') + args + ')';
  return path;
}

function fmtJoinClause(cursor: TreeCursor, ctx: FormatCtx): string {
  if (!cursor.firstChild()) return '';
  let joinType = '';
  let tableRef = '';
  let constraint = '';
  do {
    if (cursor.name === 'JoinType') joinType = fmtJoinType(cursor, ctx);
    else if (cursor.name === 'TableRef') tableRef = fmtTableRef(cursor, ctx);
    else if (cursor.name === 'JoinConstraint') constraint = fmtJoinConstraint(cursor, ctx);
  } while (cursor.nextSibling());
  cursor.parent();
  let result = ind(ctx) + (joinType ? joinType + ' ' : '') + 'JOIN ' + tableRef;
  if (constraint) result += ' ' + constraint;
  return result;
}

function fmtJoinType(cursor: TreeCursor, ctx: FormatCtx): string {
  const parts: string[] = [];
  if (cursor.firstChild()) {
    do { parts.push(kw(src(cursor, ctx))); } while (cursor.nextSibling());
    cursor.parent();
  }
  return parts.join(' ');
}

function fmtJoinConstraint(cursor: TreeCursor, ctx: FormatCtx): string {
  if (!cursor.firstChild()) return '';
  const first = cursor.name;
  if (first === 'ON') {
    cursor.nextSibling();
    const expr = uppercaseKeywords(src(cursor, ctx));
    cursor.parent();
    return 'ON ' + expr;
  }
  if (first === 'USING') {
    cursor.nextSibling(); // ParenL
    cursor.nextSibling(); // ColumnNameList
    const cols = src(cursor, ctx).replace(/\s*,\s*/g, ', ');
    cursor.parent();
    return 'USING (' + cols + ')';
  }
  cursor.parent();
  return '';
}

// ---------------------------------------------------------------------------
// WHERE / GROUP BY / HAVING / ORDER BY / LIMIT
// ---------------------------------------------------------------------------

function fmtWhereClause(cursor: TreeCursor, ctx: FormatCtx): string {
  if (!cursor.firstChild()) return '';
  cursor.nextSibling();
  const expr = uppercaseKeywords(src(cursor, ctx));
  cursor.parent();
  return ind(ctx) + 'WHERE ' + expr;
}

function fmtGroupByClause(cursor: TreeCursor, ctx: FormatCtx): string {
  if (!cursor.firstChild()) return '';
  let list = '';
  do {
    if (cursor.name === 'ExpressionList') list = fmtExpressionList(cursor, ctx);
  } while (cursor.nextSibling());
  cursor.parent();
  return ind(ctx) + 'GROUP BY ' + list;
}

function fmtHavingClause(cursor: TreeCursor, ctx: FormatCtx): string {
  if (!cursor.firstChild()) return '';
  cursor.nextSibling();
  const expr = uppercaseKeywords(src(cursor, ctx));
  cursor.parent();
  return ind(ctx) + 'HAVING ' + expr;
}

function fmtOrderByClause(cursor: TreeCursor, ctx: FormatCtx): string {
  if (!cursor.firstChild()) return '';
  const terms: string[] = [];
  do {
    if (cursor.name === 'OrderingTerm') terms.push(fmtOrderingTerm(cursor, ctx));
  } while (cursor.nextSibling());
  cursor.parent();
  return ind(ctx) + 'ORDER BY ' + terms.join(', ');
}

function fmtOrderingTerm(cursor: TreeCursor, ctx: FormatCtx): string {
  if (!cursor.firstChild()) return '';
  let expr = '';
  let dir = '';
  do {
    if (cursor.name === 'Expression') expr = uppercaseKeywords(src(cursor, ctx));
    else if (cursor.name === 'ASC' || cursor.name === 'DESC') dir = kw(src(cursor, ctx));
  } while (cursor.nextSibling());
  cursor.parent();
  return expr + (dir ? ' ' + dir : '');
}

function fmtLimitClause(cursor: TreeCursor, ctx: FormatCtx): string {
  if (!cursor.firstChild()) return '';
  const parts: string[] = [];
  do {
    if (cursor.name === 'LIMIT') parts.push('LIMIT');
    else if (cursor.name === 'OFFSET') parts.push('OFFSET');
    else if (cursor.name === 'Expression') parts.push(uppercaseKeywords(src(cursor, ctx)));
    else if (cursor.name === 'Comma') parts.push(',');
  } while (cursor.nextSibling());
  cursor.parent();
  return ind(ctx) + parts.join(' ');
}

// ---------------------------------------------------------------------------
// WITH / CTE
// ---------------------------------------------------------------------------

function fmtWithStatement(cursor: TreeCursor, ctx: FormatCtx): string {
  const parts: string[] = [];
  if (cursor.firstChild()) {
    do { parts.push(formatNode(cursor, ctx)); } while (cursor.nextSibling());
    cursor.parent();
  }
  return parts.join('\n');
}

function fmtWithClause(cursor: TreeCursor, ctx: FormatCtx): string {
  if (!cursor.firstChild()) return '';
  let recursive = false;
  const ctes: string[] = [];
  do {
    if (cursor.name === 'RECURSIVE') recursive = true;
    if (cursor.name === 'CommonTableExpression') ctes.push(fmtCTE(cursor, ctx));
  } while (cursor.nextSibling());
  cursor.parent();
  let result = ind(ctx) + 'WITH' + (recursive ? ' RECURSIVE' : '') + '\n';
  result += ctes.join(',\n');
  return result;
}

function fmtCTE(cursor: TreeCursor, ctx: FormatCtx): string {
  const nodeFrom = cursor.from;
  if (!cursor.firstChild()) return '';
  const name = ctx.sql.slice(nodeFrom, cursor.from).trim();
  let colList = '';
  let materialized = false;
  let body = '';
  do {
    if (cursor.name === 'ColumnNameList') {
      colList = src(cursor, ctx).replace(/\s*,\s*/g, ', ');
    } else if (cursor.name === 'MATERIALIZED') {
      materialized = true;
    } else if (cursor.name === 'QueryBody') {
      body = fmtQueryBody(cursor, {...ctx, depth: ctx.depth + 2});
    }
  } while (cursor.nextSibling());
  cursor.parent();
  const deeper = INDENT.repeat(ctx.depth + 1);
  let result = deeper + name;
  if (colList) result += '(' + colList + ')';
  result += ' AS' + (materialized ? ' MATERIALIZED' : '') + ' (\n';
  result += body;
  result += deeper + ')';
  return result;
}

function fmtQueryBody(cursor: TreeCursor, ctx: FormatCtx): string {
  const parts: string[] = [];
  if (cursor.firstChild()) {
    do { parts.push(formatNode(cursor, ctx)); } while (cursor.nextSibling());
    cursor.parent();
  }
  return parts.join('\n') + '\n';
}

// ---------------------------------------------------------------------------
// CREATE PERFETTO TABLE / VIEW
// ---------------------------------------------------------------------------

function fmtCreateTable(cursor: TreeCursor, ctx: FormatCtx): string {
  if (!cursor.firstChild()) return '';
  let tableEnd = 0;
  let colDefs = '';
  let body = '';
  let name = '';
  do {
    if (cursor.name === 'TABLE') tableEnd = cursor.to;
    else if (cursor.name === 'ColumnDefList') {
      if (!name) name = ctx.sql.slice(tableEnd, cursor.from).trim();
      colDefs = fmtColumnDefList(cursor, ctx);
    } else if (cursor.name === 'AS') {
      if (!name) name = ctx.sql.slice(tableEnd, cursor.from).trim();
    } else if (cursor.name === 'QueryBody') body = fmtQueryBody(cursor, ctx);
  } while (cursor.nextSibling());
  cursor.parent();
  let result = 'CREATE PERFETTO TABLE ' + name;
  if (colDefs) result += colDefs;
  result += ' AS\n' + body.trimEnd();
  return result;
}

function fmtCreateView(cursor: TreeCursor, ctx: FormatCtx): string {
  if (!cursor.firstChild()) return '';
  let viewEnd = 0;
  let name = '';
  let colDefs = '';
  let body = '';
  do {
    if (cursor.name === 'VIEW') viewEnd = cursor.to;
    else if (cursor.name === 'ColumnDefList') {
      if (!name) name = ctx.sql.slice(viewEnd, cursor.from).trim();
      colDefs = fmtColumnDefList(cursor, ctx);
    } else if (cursor.name === 'AS') {
      if (!name) name = ctx.sql.slice(viewEnd, cursor.from).trim();
    } else if (cursor.name === 'QueryBody') body = fmtQueryBody(cursor, ctx);
  } while (cursor.nextSibling());
  cursor.parent();
  let result = 'CREATE PERFETTO VIEW ' + name;
  if (colDefs) result += colDefs;
  result += ' AS\n' + body.trimEnd();
  return result;
}

// ---------------------------------------------------------------------------
// CREATE PERFETTO FUNCTION
// ---------------------------------------------------------------------------

function fmtCreateFunction(cursor: TreeCursor, ctx: FormatCtx): string {
  if (!cursor.firstChild()) return '';
  let functionEnd = 0;
  let name = '';
  let params = '';
  let returnsTable = false;
  let returnType = '';
  let returnParams = '';
  let body = '';
  let inReturns = false;
  let toEnd = 0;
  let nodeEnd = 0;
  cursor.parent();
  nodeEnd = cursor.to;
  cursor.firstChild();
  do {
    if (cursor.name === 'FUNCTION') functionEnd = cursor.to;
    else if (cursor.name === 'ParenL' && !name) {
      name = ctx.sql.slice(functionEnd, cursor.from).trim();
    } else if (cursor.name === 'FunctionParamList' && !inReturns) {
      params = fmtFunctionParamList(cursor, ctx);
    } else if (cursor.name === 'RETURNS') inReturns = true;
    else if (cursor.name === 'TABLE' && inReturns) returnsTable = true;
    else if (cursor.name === 'FunctionReturnType') {
      returnType = src(cursor, ctx).toUpperCase();
    } else if (cursor.name === 'FunctionParamList' && inReturns) {
      returnParams = fmtFunctionParamList(cursor, ctx);
    } else if (cursor.name === 'TO') toEnd = cursor.to;
    else if (cursor.name === 'QueryBody') body = fmtQueryBody(cursor, ctx);
  } while (cursor.nextSibling());
  cursor.parent();
  let delegatesTo = '';
  if (toEnd > 0) delegatesTo = ctx.sql.slice(toEnd, nodeEnd).trim();
  let result = 'CREATE PERFETTO FUNCTION ' + name + '(' + params + ')\n';
  if (returnsTable) result += 'RETURNS TABLE(' + returnParams + ')\n';
  else result += 'RETURNS ' + returnType + '\n';
  if (delegatesTo) result += 'DELEGATES TO ' + delegatesTo;
  else result += 'AS\n' + body.trimEnd();
  return result;
}

// ---------------------------------------------------------------------------
// CREATE PERFETTO MACRO
// ---------------------------------------------------------------------------

function fmtCreateMacro(cursor: TreeCursor, ctx: FormatCtx): string {
  if (!cursor.firstChild()) return '';
  let macroEnd = 0;
  let name = '';
  let params = '';
  let returnsEnd = 0;
  let returnType = '';
  let body = '';
  do {
    if (cursor.name === 'MACRO') macroEnd = cursor.to;
    else if (cursor.name === 'ParenL' && !name) {
      name = ctx.sql.slice(macroEnd, cursor.from).trim();
    } else if (cursor.name === 'MacroParamList') {
      params = fmtMacroParamList(cursor, ctx);
    } else if (cursor.name === 'RETURNS') returnsEnd = cursor.to;
    else if (cursor.name === 'AS' && returnsEnd > 0 && !returnType) {
      returnType = ctx.sql.slice(returnsEnd, cursor.from).trim();
    } else if (cursor.name === 'QueryBody') body = fmtQueryBody(cursor, ctx);
  } while (cursor.nextSibling());
  cursor.parent();
  let result = 'CREATE PERFETTO MACRO ' + name + '(' + params + ')\n';
  result += 'RETURNS ' + returnType + '\n';
  result += 'AS (\n' + body + ')';
  return result;
}

// ---------------------------------------------------------------------------
// CREATE PERFETTO INDEX
// ---------------------------------------------------------------------------

function fmtCreateIndex(cursor: TreeCursor, ctx: FormatCtx): string {
  if (!cursor.firstChild()) return '';
  let indexEnd = 0;
  let name = '';
  let onEnd = 0;
  let tableName = '';
  let cols = '';
  do {
    if (cursor.name === 'INDEX') indexEnd = cursor.to;
    else if (cursor.name === 'ON') {
      if (!name) name = ctx.sql.slice(indexEnd, cursor.from).trim();
      onEnd = cursor.to;
    } else if (cursor.name === 'ParenL' && onEnd > 0 && !tableName) {
      tableName = ctx.sql.slice(onEnd, cursor.from).trim();
    } else if (cursor.name === 'ColumnNameList') {
      cols = src(cursor, ctx).replace(/\s*,\s*/g, ', ');
    }
  } while (cursor.nextSibling());
  cursor.parent();
  return 'CREATE PERFETTO INDEX ' + name + '\n' +
    INDENT + 'ON ' + tableName + '(' + cols + ')';
}

// ---------------------------------------------------------------------------
// CREATE VIRTUAL TABLE
// ---------------------------------------------------------------------------

function fmtCreateVirtualTable(cursor: TreeCursor, ctx: FormatCtx): string {
  if (!cursor.firstChild()) return '';
  let tableEnd = 0;
  let name = '';
  let usingEnd = 0;
  let usingName = '';
  let args = '';
  do {
    if (cursor.name === 'TABLE') tableEnd = cursor.to;
    else if (cursor.name === 'USING') {
      if (!name) name = ctx.sql.slice(tableEnd, cursor.from).trim();
      usingEnd = cursor.to;
    } else if (cursor.name === 'ParenL' && usingEnd > 0 && !usingName) {
      usingName = ctx.sql.slice(usingEnd, cursor.from).trim();
    } else if (cursor.name === 'VirtualTableArgList') {
      args = fmtVirtualTableArgList(cursor, ctx);
    }
  } while (cursor.nextSibling());
  cursor.parent();
  return 'CREATE VIRTUAL TABLE ' + name + ' USING ' + usingName + '(' + args + ')';
}

// ---------------------------------------------------------------------------
// INCLUDE PERFETTO MODULE
// ---------------------------------------------------------------------------

function fmtIncludeModule(cursor: TreeCursor, ctx: FormatCtx): string {
  if (!cursor.firstChild()) return '';
  let path = '';
  do {
    if (cursor.name === 'ModulePath') path = src(cursor, ctx);
  } while (cursor.nextSibling());
  cursor.parent();
  return 'INCLUDE PERFETTO MODULE ' + path;
}

// ---------------------------------------------------------------------------
// Column definitions
// ---------------------------------------------------------------------------

function fmtColumnDefList(cursor: TreeCursor, ctx: FormatCtx): string {
  const defs: string[] = [];
  if (cursor.firstChild()) {
    do {
      if (cursor.name === 'ColumnDef') defs.push(fmtColumnDef(cursor, ctx));
    } while (cursor.nextSibling());
    cursor.parent();
  }
  return '(\n' + defs.map((d) => INDENT + d).join(',\n') + '\n)';
}

function fmtColumnDef(cursor: TreeCursor, ctx: FormatCtx): string {
  const nodeFrom = cursor.from;
  if (!cursor.firstChild()) return '';
  let name = '';
  let type = '';
  do {
    if (cursor.name === 'ColumnType') {
      if (!name) name = ctx.sql.slice(nodeFrom, cursor.from).trim();
      type = fmtColumnType(cursor, ctx);
    }
  } while (cursor.nextSibling());
  cursor.parent();
  return name + ' ' + type;
}

function fmtColumnType(cursor: TreeCursor, ctx: FormatCtx): string {
  const nodeFrom = cursor.from;
  if (!cursor.firstChild()) {
    return src(cursor, ctx).toUpperCase();
  }
  let path = '';
  let typeName = '';
  do {
    if (cursor.name === 'ParenL' && !typeName) {
      typeName = ctx.sql.slice(nodeFrom, cursor.from).trim().toUpperCase();
    } else if (cursor.name === 'IdentifierPath') {
      if (!typeName) typeName = ctx.sql.slice(nodeFrom, cursor.from).trim().toUpperCase();
      path = src(cursor, ctx);
    }
  } while (cursor.nextSibling());
  cursor.parent();
  if (path) return typeName + '(' + path + ')';
  if (!typeName) return ctx.sql.slice(nodeFrom, cursor.to).trim().toUpperCase();
  return typeName;
}

// ---------------------------------------------------------------------------
// Function / Macro params
// ---------------------------------------------------------------------------

function fmtFunctionParamList(cursor: TreeCursor, ctx: FormatCtx): string {
  const params: string[] = [];
  if (cursor.firstChild()) {
    do {
      if (cursor.name === 'FunctionParam') params.push(fmtFunctionParam(cursor, ctx));
    } while (cursor.nextSibling());
    cursor.parent();
  }
  return params.join(', ');
}

function fmtFunctionParam(cursor: TreeCursor, ctx: FormatCtx): string {
  const nodeFrom = cursor.from;
  if (!cursor.firstChild()) return '';
  let name = '';
  let type = '';
  do {
    if (cursor.name === 'ColumnType') {
      if (!name) name = ctx.sql.slice(nodeFrom, cursor.from).trim();
      type = fmtColumnType(cursor, ctx);
    }
  } while (cursor.nextSibling());
  cursor.parent();
  return name + ' ' + type;
}

function fmtMacroParamList(cursor: TreeCursor, ctx: FormatCtx): string {
  const params: string[] = [];
  if (cursor.firstChild()) {
    do {
      if (cursor.name === 'MacroParam') params.push(src(cursor, ctx));
    } while (cursor.nextSibling());
    cursor.parent();
  }
  return params.join(', ');
}

// ---------------------------------------------------------------------------
// Virtual table args
// ---------------------------------------------------------------------------

function fmtVirtualTableArgList(cursor: TreeCursor, ctx: FormatCtx): string {
  const args: string[] = [];
  if (cursor.firstChild()) {
    do {
      if (cursor.name === 'VirtualTableArg') args.push(kw(src(cursor, ctx)));
    } while (cursor.nextSibling());
    cursor.parent();
  }
  return args.join(', ');
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

function fmtExpressionList(cursor: TreeCursor, ctx: FormatCtx): string {
  const exprs: string[] = [];
  if (cursor.firstChild()) {
    do {
      if (cursor.name === 'Expression') {
        exprs.push(uppercaseKeywords(src(cursor, ctx)));
      }
    } while (cursor.nextSibling());
    cursor.parent();
  }
  return exprs.join(', ');
}

function fmtParenExpr(cursor: TreeCursor, ctx: FormatCtx): string {
  if (!cursor.firstChild()) return '()';
  if (cursor.name === 'ParenL') {
    cursor.nextSibling();
    // Cast to string to prevent TS from narrowing cursor.name to 'ParenL'
    // after the check above — nextSibling() mutates the cursor position.
    const innerName: string = cursor.name;
    if (innerName === 'QueryBody') {
      const inner = fmtQueryBody(cursor, {...ctx, depth: ctx.depth + 1});
      cursor.parent();
      return '(\n' + inner + ind(ctx) + ')';
    }
    if (innerName === 'ExpressionList') {
      const list = fmtExpressionList(cursor, ctx);
      cursor.parent();
      return '(' + list + ')';
    }
    cursor.parent();
    return '()';
  }
  cursor.parent();
  return uppercaseKeywords(src(cursor, ctx));
}
