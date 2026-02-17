// PerfettoSQL Formatter — walks the Lezer AST and emits formatted SQL.

import {parser} from './parser.js';

const INDENT = '  ';

// Keywords that should always be uppercased.
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

/**
 * Format a PerfettoSQL string.
 * @param {string} sql - The SQL to format.
 * @returns {string} - Formatted SQL.
 */
export function formatSQL(sql) {
  const tree = parser.parse(sql);
  const cursor = tree.cursor();
  const ctx = {sql, depth: 0};
  const result = formatNode(cursor, ctx);
  return result.trim() + '\n';
}

/**
 * Get the raw source text for the current node.
 */
function src(cursor, ctx) {
  return ctx.sql.slice(cursor.from, cursor.to);
}

/**
 * Return indentation string for current depth.
 */
function indent(ctx) {
  return INDENT.repeat(ctx.depth);
}

/**
 * Uppercase a keyword if it's a known SQL keyword.
 */
function kw(text) {
  if (KEYWORDS.has(text.toLowerCase())) {
    return text.toUpperCase();
  }
  return text;
}

/**
 * Main recursive formatting dispatcher.
 */
function formatNode(cursor, ctx) {
  const name = cursor.name;

  switch (name) {
    case 'Program': return formatProgram(cursor, ctx);
    case 'Statement': return formatStatement(cursor, ctx);

    case 'SelectStatement': return formatSelectStatement(cursor, ctx);
    case 'SelectBody': return formatSelectBody(cursor, ctx);
    case 'SelectClauses': return formatSelectClauses(cursor, ctx);
    case 'SelectColumns': return formatSelectColumns(cursor, ctx);
    case 'SelectColumn': return formatSelectColumn(cursor, ctx);
    case 'SetOperation': return formatSetOperation(cursor, ctx);

    case 'FromClause': return formatFromClause(cursor, ctx);
    case 'TableRef': return formatTableRef(cursor, ctx);
    case 'TableSource': return formatTableSource(cursor, ctx);
    case 'IdentifierPath': return formatIdentifierPath(cursor, ctx);
    case 'JoinClause': return formatJoinClause(cursor, ctx);
    case 'JoinType': return formatJoinType(cursor, ctx);
    case 'JoinConstraint': return formatJoinConstraint(cursor, ctx);

    case 'WhereClause': return formatWhereClause(cursor, ctx);
    case 'GroupByClause': return formatGroupByClause(cursor, ctx);
    case 'HavingClause': return formatHavingClause(cursor, ctx);
    case 'OrderByClause': return formatOrderByClause(cursor, ctx);
    case 'OrderingTerm': return formatOrderingTerm(cursor, ctx);
    case 'LimitClause': return formatLimitClause(cursor, ctx);

    case 'WithStatement': return formatWithStatement(cursor, ctx);
    case 'WithClause': return formatWithClause(cursor, ctx);
    case 'CommonTableExpression': return formatCTE(cursor, ctx);
    case 'QueryBody': return formatQueryBody(cursor, ctx);

    case 'CreatePerfettoTableStatement': return formatCreatePerfettoTable(cursor, ctx);
    case 'CreatePerfettoViewStatement': return formatCreatePerfettoView(cursor, ctx);
    case 'CreatePerfettoFunctionStatement': return formatCreatePerfettoFunction(cursor, ctx);
    case 'CreatePerfettoMacroStatement': return formatCreatePerfettoMacro(cursor, ctx);
    case 'CreatePerfettoIndexStatement': return formatCreatePerfettoIndex(cursor, ctx);
    case 'CreateVirtualTableStatement': return formatCreateVirtualTable(cursor, ctx);
    case 'IncludeModuleStatement': return formatIncludeModule(cursor, ctx);

    case 'ColumnDefList': return formatColumnDefList(cursor, ctx);
    case 'ColumnDef': return formatColumnDef(cursor, ctx);
    case 'ColumnType': return formatColumnType(cursor, ctx);
    case 'ColumnNameList': return formatColumnNameList(cursor, ctx);

    case 'FunctionParamList': return formatFunctionParamList(cursor, ctx);
    case 'FunctionParam': return formatFunctionParam(cursor, ctx);
    case 'FunctionReturnType': return formatLeaf(cursor, ctx);
    case 'MacroParamList': return formatMacroParamList(cursor, ctx);
    case 'MacroParam': return formatMacroParam(cursor, ctx);

    case 'VirtualTableArgList': return formatVirtualTableArgList(cursor, ctx);
    case 'VirtualTableArg': return formatLeaf(cursor, ctx);

    case 'ModulePath': return formatIdentifierPath(cursor, ctx);

    case 'Expression': return formatExpression(cursor, ctx);
    case 'ExpressionList': return formatExpressionList(cursor, ctx);
    case 'ArgList': return formatExpressionList(cursor, ctx);

    case 'FunctionCall': return formatFunctionCall(cursor, ctx);
    case 'MacroInvocation': return formatMacroInvocation(cursor, ctx);
    case 'WindowOver': return formatWindowOver(cursor, ctx);
    case 'WindowBody': return formatWindowBody(cursor, ctx);
    case 'ParenExpr': return formatParenExpr(cursor, ctx);
    case 'CaseExpr': return formatCaseExpr(cursor, ctx);
    case 'CastExpr': return formatCastExpr(cursor, ctx);
    case 'ExistsExpr': return formatExistsExpr(cursor, ctx);

    default:
      return formatLeaf(cursor, ctx);
  }
}

/**
 * Format a leaf node (token) — just return its uppercased source text.
 */
function formatLeaf(cursor, ctx) {
  const text = src(cursor, ctx);
  return kw(text);
}

// ---------------------------------------------------------------------------
// Program / Statement
// ---------------------------------------------------------------------------

function formatProgram(cursor, ctx) {
  const parts = [];
  if (cursor.firstChild()) {
    do {
      parts.push(formatNode(cursor, ctx));
    } while (cursor.nextSibling());
    cursor.parent();
  }
  return parts.join('\n');
}

function formatStatement(cursor, ctx) {
  // Statement wraps one child (the actual statement) plus optional Semi
  if (!cursor.firstChild()) return '';
  let result = formatNode(cursor, ctx);
  // Check for semicolon
  while (cursor.nextSibling()) {
    if (cursor.name === 'Semi') {
      result += ';';
    }
  }
  cursor.parent();
  return result;
}

// ---------------------------------------------------------------------------
// SELECT
// ---------------------------------------------------------------------------

function formatSelectStatement(cursor, ctx) {
  // SelectBody SetOperation*
  const parts = [];
  if (cursor.firstChild()) {
    do {
      parts.push(formatNode(cursor, ctx));
    } while (cursor.nextSibling());
    cursor.parent();
  }
  return parts.join('\n');
}

function formatSelectBody(cursor, ctx) {
  // SELECT DISTINCT? SelectColumns SelectClauses
  if (!cursor.firstChild()) return '';
  const parts = [];
  let selectKw = '';
  let distinct = false;
  let columns = '';
  let clauses = '';

  do {
    switch (cursor.name) {
      case 'SELECT': selectKw = 'SELECT'; break;
      case 'DISTINCT': distinct = true; break;
      case 'SelectColumns': columns = formatSelectColumns(cursor, ctx); break;
      case 'SelectClauses': clauses = formatSelectClauses(cursor, ctx); break;
    }
  } while (cursor.nextSibling());
  cursor.parent();

  let result = indent(ctx) + selectKw + (distinct ? ' DISTINCT' : '') + '\n';
  result += columns.trimEnd();
  result += clauses;
  return result;
}

function formatSelectColumns(cursor, ctx) {
  // SelectColumn (Comma SelectColumn)*
  const cols = [];
  if (cursor.firstChild()) {
    do {
      if (cursor.name === 'SelectColumn') {
        cols.push(formatSelectColumn(cursor, ctx));
      }
    } while (cursor.nextSibling());
    cursor.parent();
  }
  const deeper = ctx.depth + 1;
  return cols.map((c, i) =>
    INDENT.repeat(deeper) + c + (i < cols.length - 1 ? ',' : '')
  ).join('\n') + '\n';
}

function formatSelectColumn(cursor, ctx) {
  // Star | Expression (AS identifier)?
  // Note: the alias identifier after AS is anonymous and invisible in the tree.
  // We extract it from the source text after the AS keyword.
  const nodeEnd = cursor.to;
  if (!cursor.firstChild()) return '';
  let expr = '';
  let asEnd = -1;
  do {
    if (cursor.name === 'Star') {
      expr = '*';
    } else if (cursor.name === 'Expression') {
      expr = formatExpression(cursor, ctx);
    } else if (cursor.name === 'AS') {
      asEnd = cursor.to;
    }
  } while (cursor.nextSibling());
  cursor.parent();

  if (asEnd >= 0) {
    const alias = ctx.sql.slice(asEnd, nodeEnd).trim();
    return expr + ' AS ' + alias;
  }
  return expr;
}

function formatSelectClauses(cursor, ctx) {
  // (FromClause | WhereClause | GroupByClause | ...)*
  const parts = [];
  if (cursor.firstChild()) {
    do {
      parts.push(formatNode(cursor, ctx));
    } while (cursor.nextSibling());
    cursor.parent();
  }
  if (parts.length > 0) return '\n' + parts.join('\n');
  return '';
}

function formatSetOperation(cursor, ctx) {
  // (UNION ALL? | INTERSECT | EXCEPT) SelectBody
  if (!cursor.firstChild()) return '';
  const kwParts = [];
  let body = '';
  do {
    if (cursor.name === 'SelectBody') {
      body = formatSelectBody(cursor, ctx);
    } else if (cursor.name !== 'Comma' && cursor.name !== 'Semi') {
      kwParts.push(kw(src(cursor, ctx)));
    }
  } while (cursor.nextSibling());
  cursor.parent();
  return indent(ctx) + kwParts.join(' ') + '\n' + body;
}

// ---------------------------------------------------------------------------
// FROM / JOIN
// ---------------------------------------------------------------------------

function formatFromClause(cursor, ctx) {
  // FROM TableRef JoinClause*
  if (!cursor.firstChild()) return '';
  let result = indent(ctx) + 'FROM';
  let first = true;
  do {
    if (cursor.name === 'FROM') continue;
    if (cursor.name === 'TableRef') {
      result += ' ' + formatTableRef(cursor, ctx);
    } else if (cursor.name === 'JoinClause') {
      result += '\n' + formatJoinClause(cursor, ctx);
    }
  } while (cursor.nextSibling());
  cursor.parent();
  return result;
}

function formatTableRef(cursor, ctx) {
  // TableSource (AS identifier)?
  // The alias identifier after AS is anonymous.
  const nodeEnd = cursor.to;
  if (!cursor.firstChild()) return '';
  let source = '';
  let asEnd = -1;
  do {
    if (cursor.name === 'TableSource') {
      source = formatTableSource(cursor, ctx);
    } else if (cursor.name === 'AS') {
      asEnd = cursor.to;
    }
  } while (cursor.nextSibling());
  cursor.parent();
  if (asEnd >= 0) {
    const alias = ctx.sql.slice(asEnd, nodeEnd).trim();
    return source + ' AS ' + alias;
  }
  return source;
}

function formatTableSource(cursor, ctx) {
  // ParenL QueryBody ParenR | IdentifierPath (BangL|ParenL) ArgList? ParenR | MacroVariable | IdentifierPath
  if (!cursor.firstChild()) return '';
  const firstChild = cursor.name;

  if (firstChild === 'ParenL') {
    // Subquery
    cursor.nextSibling(); // QueryBody
    const inner = formatQueryBody(cursor, {...ctx, depth: ctx.depth + 1});
    cursor.nextSibling(); // ParenR
    cursor.parent();
    return '(\n' + inner + indent(ctx) + ')';
  }

  if (firstChild === 'MacroVariable') {
    const text = src(cursor, ctx);
    cursor.parent();
    return text;
  }

  // IdentifierPath, possibly followed by (BangL|ParenL) ArgList? ParenR
  let path = formatIdentifierPath(cursor, ctx);
  let isMacro = false;
  let args = '';
  let hasParen = false;

  while (cursor.nextSibling()) {
    if (cursor.name === 'BangL') {
      isMacro = true;
      hasParen = true;
    } else if (cursor.name === 'ParenL') {
      hasParen = true;
    } else if (cursor.name === 'ArgList') {
      args = formatExpressionList(cursor, ctx);
    } else if (cursor.name === 'ParenR') {
      // end
    }
  }
  cursor.parent();

  if (hasParen) {
    return path + (isMacro ? '!(' : '(') + args + ')';
  }
  return path;
}

function formatIdentifierPath(cursor, ctx) {
  // identifier (Dot identifier)* — identifiers are anonymous tokens,
  // so just return the raw source text.
  return src(cursor, ctx);
}

function formatJoinClause(cursor, ctx) {
  // JoinType? JOIN TableRef JoinConstraint?
  if (!cursor.firstChild()) return '';
  let joinType = '';
  let tableRef = '';
  let constraint = '';
  do {
    if (cursor.name === 'JoinType') {
      joinType = formatJoinType(cursor, ctx);
    } else if (cursor.name === 'JOIN') {
      // skip keyword, we add it
    } else if (cursor.name === 'TableRef') {
      tableRef = formatTableRef(cursor, ctx);
    } else if (cursor.name === 'JoinConstraint') {
      constraint = formatJoinConstraint(cursor, ctx);
    }
  } while (cursor.nextSibling());
  cursor.parent();
  let result = indent(ctx) + (joinType ? joinType + ' ' : '') + 'JOIN ' + tableRef;
  if (constraint) result += ' ' + constraint;
  return result;
}

function formatJoinType(cursor, ctx) {
  const parts = [];
  if (cursor.firstChild()) {
    do {
      parts.push(kw(src(cursor, ctx)));
    } while (cursor.nextSibling());
    cursor.parent();
  }
  return parts.join(' ');
}

function formatJoinConstraint(cursor, ctx) {
  // ON Expression | USING ParenL ColumnNameList ParenR
  if (!cursor.firstChild()) return '';
  const first = cursor.name;
  if (first === 'ON') {
    cursor.nextSibling();
    const expr = formatExpression(cursor, ctx);
    cursor.parent();
    return 'ON ' + expr;
  }
  if (first === 'USING') {
    cursor.nextSibling(); // ParenL
    cursor.nextSibling(); // ColumnNameList
    const cols = formatColumnNameList(cursor, ctx);
    cursor.parent();
    return 'USING (' + cols + ')';
  }
  cursor.parent();
  return '';
}

// ---------------------------------------------------------------------------
// WHERE / GROUP BY / HAVING / ORDER BY / LIMIT
// ---------------------------------------------------------------------------

function formatWhereClause(cursor, ctx) {
  if (!cursor.firstChild()) return '';
  cursor.nextSibling(); // Expression
  const expr = formatExpression(cursor, ctx);
  cursor.parent();
  return indent(ctx) + 'WHERE ' + expr;
}

function formatGroupByClause(cursor, ctx) {
  if (!cursor.firstChild()) return '';
  // GROUP BY ExpressionList
  let list = '';
  do {
    if (cursor.name === 'ExpressionList') {
      list = formatExpressionList(cursor, ctx);
    }
  } while (cursor.nextSibling());
  cursor.parent();
  return indent(ctx) + 'GROUP BY ' + list;
}

function formatHavingClause(cursor, ctx) {
  if (!cursor.firstChild()) return '';
  cursor.nextSibling(); // Expression
  const expr = formatExpression(cursor, ctx);
  cursor.parent();
  return indent(ctx) + 'HAVING ' + expr;
}

function formatOrderByClause(cursor, ctx) {
  if (!cursor.firstChild()) return '';
  const terms = [];
  do {
    if (cursor.name === 'OrderingTerm') {
      terms.push(formatOrderingTerm(cursor, ctx));
    }
  } while (cursor.nextSibling());
  cursor.parent();
  return indent(ctx) + 'ORDER BY ' + terms.join(', ');
}

function formatOrderingTerm(cursor, ctx) {
  if (!cursor.firstChild()) return '';
  let expr = '';
  let dir = '';
  do {
    if (cursor.name === 'Expression') {
      expr = formatExpression(cursor, ctx);
    } else if (cursor.name === 'ASC' || cursor.name === 'DESC') {
      dir = kw(src(cursor, ctx));
    }
  } while (cursor.nextSibling());
  cursor.parent();
  return expr + (dir ? ' ' + dir : '');
}

function formatLimitClause(cursor, ctx) {
  if (!cursor.firstChild()) return '';
  const parts = [];
  do {
    if (cursor.name === 'LIMIT') {
      parts.push('LIMIT');
    } else if (cursor.name === 'OFFSET') {
      parts.push('OFFSET');
    } else if (cursor.name === 'Expression') {
      parts.push(formatExpression(cursor, ctx));
    } else if (cursor.name === 'Comma') {
      parts.push(',');
    }
  } while (cursor.nextSibling());
  cursor.parent();
  return indent(ctx) + parts.join(' ');
}

// ---------------------------------------------------------------------------
// WITH / CTE
// ---------------------------------------------------------------------------

function formatWithStatement(cursor, ctx) {
  // WithClause SelectBody SetOperation*
  const parts = [];
  if (cursor.firstChild()) {
    do {
      parts.push(formatNode(cursor, ctx));
    } while (cursor.nextSibling());
    cursor.parent();
  }
  return parts.join('\n');
}

function formatWithClause(cursor, ctx) {
  // WITH RECURSIVE? CommonTableExpression (Comma CommonTableExpression)*
  if (!cursor.firstChild()) return '';
  let recursive = false;
  const ctes = [];
  do {
    if (cursor.name === 'RECURSIVE') recursive = true;
    if (cursor.name === 'CommonTableExpression') {
      ctes.push(formatCTE(cursor, ctx));
    }
  } while (cursor.nextSibling());
  cursor.parent();

  let result = indent(ctx) + 'WITH' + (recursive ? ' RECURSIVE' : '') + '\n';
  result += ctes.join(',\n');
  return result;
}

function formatCTE(cursor, ctx) {
  // identifier (ParenL ColumnNameList ParenR)? AS MATERIALIZED? ParenL QueryBody ParenR
  // The CTE name is an anonymous identifier at the start of the node.
  const nodeFrom = cursor.from;
  if (!cursor.firstChild()) return '';

  // Extract name from start of node to first named child
  const name = ctx.sql.slice(nodeFrom, cursor.from).trim();

  let colList = '';
  let materialized = false;
  let body = '';
  do {
    if (cursor.name === 'ColumnNameList') {
      colList = formatColumnNameList(cursor, ctx);
    } else if (cursor.name === 'MATERIALIZED') {
      materialized = true;
    } else if (cursor.name === 'QueryBody') {
      body = formatQueryBody(cursor, {...ctx, depth: ctx.depth + 2});
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

function formatQueryBody(cursor, ctx) {
  // WithClause? SelectBody SetOperation*
  const parts = [];
  if (cursor.firstChild()) {
    do {
      parts.push(formatNode(cursor, ctx));
    } while (cursor.nextSibling());
    cursor.parent();
  }
  return parts.join('\n') + '\n';
}

// ---------------------------------------------------------------------------
// CREATE PERFETTO TABLE / VIEW
// ---------------------------------------------------------------------------

function formatCreatePerfettoTable(cursor, ctx) {
  // CREATE PERFETTO TABLE identifier ColumnDefList? AS QueryBody
  // Table name is anonymous between TABLE and the next named child.
  if (!cursor.firstChild()) return '';
  let tableEnd = 0;
  let colDefs = '';
  let body = '';
  let name = '';
  do {
    if (cursor.name === 'TABLE') {
      tableEnd = cursor.to;
    } else if (cursor.name === 'ColumnDefList') {
      if (!name) name = ctx.sql.slice(tableEnd, cursor.from).trim();
      colDefs = formatColumnDefList(cursor, ctx);
    } else if (cursor.name === 'AS') {
      if (!name) name = ctx.sql.slice(tableEnd, cursor.from).trim();
    } else if (cursor.name === 'QueryBody') {
      body = formatQueryBody(cursor, ctx);
    }
  } while (cursor.nextSibling());
  cursor.parent();
  let result = 'CREATE PERFETTO TABLE ' + name;
  if (colDefs) result += colDefs;
  result += ' AS\n' + body.trimEnd();
  return result;
}

function formatCreatePerfettoView(cursor, ctx) {
  if (!cursor.firstChild()) return '';
  let viewEnd = 0;
  let name = '';
  let colDefs = '';
  let body = '';
  do {
    if (cursor.name === 'VIEW') {
      viewEnd = cursor.to;
    } else if (cursor.name === 'ColumnDefList') {
      if (!name) name = ctx.sql.slice(viewEnd, cursor.from).trim();
      colDefs = formatColumnDefList(cursor, ctx);
    } else if (cursor.name === 'AS') {
      if (!name) name = ctx.sql.slice(viewEnd, cursor.from).trim();
    } else if (cursor.name === 'QueryBody') {
      body = formatQueryBody(cursor, ctx);
    }
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

function formatCreatePerfettoFunction(cursor, ctx) {
  // CREATE PERFETTO FUNCTION identifier ParenL FunctionParamList? ParenR
  // RETURNS (FunctionReturnType | TABLE ParenL FunctionParamList ParenR)
  // (AS QueryBody | DELEGATES TO identifier)
  if (!cursor.firstChild()) return '';
  let functionEnd = 0;
  let name = '';
  let params = '';
  let returnsTable = false;
  let returnType = '';
  let returnParams = '';
  let body = '';
  let inReturns = false;
  let delegatesTo = '';
  let toEnd = 0;
  let nodeEnd = 0;

  // Save total node end for extracting delegates target
  cursor.parent();
  nodeEnd = cursor.to;
  cursor.firstChild();

  do {
    if (cursor.name === 'FUNCTION') {
      functionEnd = cursor.to;
    } else if (cursor.name === 'ParenL' && !name) {
      name = ctx.sql.slice(functionEnd, cursor.from).trim();
    } else if (cursor.name === 'FunctionParamList' && !inReturns) {
      params = formatFunctionParamList(cursor, ctx);
    } else if (cursor.name === 'RETURNS') {
      inReturns = true;
    } else if (cursor.name === 'TABLE' && inReturns) {
      returnsTable = true;
    } else if (cursor.name === 'FunctionReturnType') {
      returnType = src(cursor, ctx).toUpperCase();
    } else if (cursor.name === 'FunctionParamList' && inReturns) {
      returnParams = formatFunctionParamList(cursor, ctx);
    } else if (cursor.name === 'TO') {
      toEnd = cursor.to;
    } else if (cursor.name === 'QueryBody') {
      body = formatQueryBody(cursor, ctx);
    }
  } while (cursor.nextSibling());
  cursor.parent();

  if (toEnd > 0) {
    delegatesTo = ctx.sql.slice(toEnd, nodeEnd).trim();
  }

  let result = 'CREATE PERFETTO FUNCTION ' + name + '(' + params + ')\n';
  if (returnsTable) {
    result += 'RETURNS TABLE(' + returnParams + ')\n';
  } else {
    result += 'RETURNS ' + returnType + '\n';
  }
  if (delegatesTo) {
    result += 'DELEGATES TO ' + delegatesTo;
  } else {
    result += 'AS\n' + body.trimEnd();
  }
  return result;
}

// ---------------------------------------------------------------------------
// CREATE PERFETTO MACRO
// ---------------------------------------------------------------------------

function formatCreatePerfettoMacro(cursor, ctx) {
  // CREATE PERFETTO MACRO identifier ParenL MacroParamList? ParenR
  // RETURNS identifier AS ParenL QueryBody ParenR
  // The macro name and return type are anonymous identifiers.
  if (!cursor.firstChild()) return '';
  let macroEnd = 0;
  let name = '';
  let params = '';
  let returnsEnd = 0;
  let returnType = '';
  let body = '';

  do {
    if (cursor.name === 'MACRO') {
      macroEnd = cursor.to;
    } else if (cursor.name === 'ParenL' && !name) {
      name = ctx.sql.slice(macroEnd, cursor.from).trim();
    } else if (cursor.name === 'MacroParamList') {
      params = formatMacroParamList(cursor, ctx);
    } else if (cursor.name === 'RETURNS') {
      returnsEnd = cursor.to;
    } else if (cursor.name === 'AS' && returnsEnd > 0 && !returnType) {
      returnType = ctx.sql.slice(returnsEnd, cursor.from).trim();
    } else if (cursor.name === 'QueryBody') {
      body = formatQueryBody(cursor, ctx);
    }
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

function formatCreatePerfettoIndex(cursor, ctx) {
  // CREATE PERFETTO INDEX identifier ON identifier ParenL ColumnNameList ParenR
  // Both the index name and table name are anonymous.
  if (!cursor.firstChild()) return '';
  let indexEnd = 0;
  let name = '';
  let onEnd = 0;
  let tableName = '';
  let cols = '';

  do {
    if (cursor.name === 'INDEX') {
      indexEnd = cursor.to;
    } else if (cursor.name === 'ON') {
      if (!name) name = ctx.sql.slice(indexEnd, cursor.from).trim();
      onEnd = cursor.to;
    } else if (cursor.name === 'ParenL' && onEnd > 0 && !tableName) {
      tableName = ctx.sql.slice(onEnd, cursor.from).trim();
    } else if (cursor.name === 'ColumnNameList') {
      cols = formatColumnNameList(cursor, ctx);
    }
  } while (cursor.nextSibling());
  cursor.parent();

  return 'CREATE PERFETTO INDEX ' + name + '\n' +
    INDENT + 'ON ' + tableName + '(' + cols + ')';
}

// ---------------------------------------------------------------------------
// CREATE VIRTUAL TABLE
// ---------------------------------------------------------------------------

function formatCreateVirtualTable(cursor, ctx) {
  // CREATE VIRTUAL TABLE identifier USING identifier ParenL VirtualTableArgList? ParenR
  if (!cursor.firstChild()) return '';
  let tableEnd = 0;
  let name = '';
  let usingEnd = 0;
  let usingName = '';
  let args = '';

  do {
    if (cursor.name === 'TABLE') {
      tableEnd = cursor.to;
    } else if (cursor.name === 'USING') {
      if (!name) name = ctx.sql.slice(tableEnd, cursor.from).trim();
      usingEnd = cursor.to;
    } else if (cursor.name === 'ParenL' && usingEnd > 0 && !usingName) {
      usingName = ctx.sql.slice(usingEnd, cursor.from).trim();
    } else if (cursor.name === 'VirtualTableArgList') {
      args = formatVirtualTableArgList(cursor, ctx);
    }
  } while (cursor.nextSibling());
  cursor.parent();

  return 'CREATE VIRTUAL TABLE ' + name + ' USING ' + usingName + '(' + args + ')';
}

// ---------------------------------------------------------------------------
// INCLUDE PERFETTO MODULE
// ---------------------------------------------------------------------------

function formatIncludeModule(cursor, ctx) {
  if (!cursor.firstChild()) return '';
  let path = '';
  do {
    if (cursor.name === 'ModulePath') {
      path = formatIdentifierPath(cursor, ctx);
    }
  } while (cursor.nextSibling());
  cursor.parent();
  return 'INCLUDE PERFETTO MODULE ' + path;
}

// ---------------------------------------------------------------------------
// Column definitions
// ---------------------------------------------------------------------------

function formatColumnDefList(cursor, ctx) {
  const defs = [];
  if (cursor.firstChild()) {
    do {
      if (cursor.name === 'ColumnDef') {
        defs.push(formatColumnDef(cursor, ctx));
      }
    } while (cursor.nextSibling());
    cursor.parent();
  }
  return '(\n' + defs.map(d => INDENT + d).join(',\n') + '\n)';
}

function formatColumnDef(cursor, ctx) {
  // identifier ColumnType — identifier is anonymous
  const nodeFrom = cursor.from;
  if (!cursor.firstChild()) return '';
  let type = '';
  let name = '';
  do {
    if (cursor.name === 'ColumnType') {
      if (!name) name = ctx.sql.slice(nodeFrom, cursor.from).trim();
      type = formatColumnType(cursor, ctx);
    }
  } while (cursor.nextSibling());
  cursor.parent();
  return name + ' ' + type;
}

function formatColumnType(cursor, ctx) {
  // identifier (ParenL IdentifierPath ParenR)?
  // The type name identifier is anonymous. Check if there's a child IdentifierPath.
  const nodeFrom = cursor.from;
  if (!cursor.firstChild()) {
    // No children — the whole node is a simple type name
    return src(cursor, ctx).toUpperCase();
  }
  // First child exists — find IdentifierPath
  let path = '';
  let typeName = '';
  do {
    if (cursor.name === 'ParenL' && !typeName) {
      typeName = ctx.sql.slice(nodeFrom, cursor.from).trim().toUpperCase();
    } else if (cursor.name === 'IdentifierPath') {
      if (!typeName) typeName = ctx.sql.slice(nodeFrom, cursor.from).trim().toUpperCase();
      path = formatIdentifierPath(cursor, ctx);
    }
  } while (cursor.nextSibling());
  cursor.parent();
  if (path) return typeName + '(' + path + ')';
  // If we only had anonymous children (no ParenL), it's a simple type
  if (!typeName) return ctx.sql.slice(nodeFrom, cursor.to).trim().toUpperCase();
  return typeName;
}

function formatColumnNameList(cursor, ctx) {
  // identifier (Comma identifier)* — identifiers are anonymous.
  // Just return the raw source with normalized spacing.
  return src(cursor, ctx).replace(/\s*,\s*/g, ', ');
}

// ---------------------------------------------------------------------------
// Function / Macro params
// ---------------------------------------------------------------------------

function formatFunctionParamList(cursor, ctx) {
  const params = [];
  if (cursor.firstChild()) {
    do {
      if (cursor.name === 'FunctionParam') {
        params.push(formatFunctionParam(cursor, ctx));
      }
    } while (cursor.nextSibling());
    cursor.parent();
  }
  return params.join(', ');
}

function formatFunctionParam(cursor, ctx) {
  // identifier ColumnType — identifier is anonymous
  const nodeFrom = cursor.from;
  if (!cursor.firstChild()) return '';
  let name = '';
  let type = '';
  do {
    if (cursor.name === 'ColumnType') {
      if (!name) name = ctx.sql.slice(nodeFrom, cursor.from).trim();
      type = formatColumnType(cursor, ctx);
    }
  } while (cursor.nextSibling());
  cursor.parent();
  return name + ' ' + type;
}

function formatMacroParamList(cursor, ctx) {
  const params = [];
  if (cursor.firstChild()) {
    do {
      if (cursor.name === 'MacroParam') {
        params.push(formatMacroParam(cursor, ctx));
      }
    } while (cursor.nextSibling());
    cursor.parent();
  }
  return params.join(', ');
}

function formatMacroParam(cursor, ctx) {
  // identifier identifier — both are anonymous
  // Just return the raw source text.
  return src(cursor, ctx);
}

// ---------------------------------------------------------------------------
// Virtual table args
// ---------------------------------------------------------------------------

function formatVirtualTableArgList(cursor, ctx) {
  const args = [];
  if (cursor.firstChild()) {
    do {
      if (cursor.name === 'VirtualTableArg') {
        args.push(formatLeaf(cursor, ctx));
      }
    } while (cursor.nextSibling());
    cursor.parent();
  }
  return args.join(', ');
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

function formatExpression(cursor, ctx) {
  // Expressions contain many anonymous identifier tokens that are invisible
  // in the tree cursor. Use the raw source text and uppercase keywords via regex.
  const text = src(cursor, ctx);
  return uppercaseKeywords(text);
}

/**
 * Uppercase SQL keywords in a raw expression string.
 * Only matches whole words that are known keywords.
 */
function uppercaseKeywords(text) {
  return text
    .replace(/\s+/g, ' ')
    .trim()
    .replace(/\b([a-zA-Z_]+)\b/g, (match) => {
      if (KEYWORDS.has(match.toLowerCase())) {
        return match.toUpperCase();
      }
      return match;
    });
}

function formatExpressionList(cursor, ctx) {
  const exprs = [];
  if (cursor.firstChild()) {
    do {
      if (cursor.name === 'Expression') {
        exprs.push(formatExpression(cursor, ctx));
      }
    } while (cursor.nextSibling());
    cursor.parent();
  }
  return exprs.join(', ');
}

// ---------------------------------------------------------------------------
// Function call / Macro invocation
// ---------------------------------------------------------------------------

function formatFunctionCall(cursor, ctx) {
  return uppercaseKeywords(src(cursor, ctx));
}

function formatMacroInvocation(cursor, ctx) {
  return uppercaseKeywords(src(cursor, ctx));
}

function formatWindowOver(cursor, ctx) {
  return uppercaseKeywords(src(cursor, ctx));
}

function formatWindowBody(cursor, ctx) {
  return uppercaseKeywords(src(cursor, ctx));
}

function formatParenExpr(cursor, ctx) {
  // Could be subquery or expression list
  // Check if it contains QueryBody
  if (!cursor.firstChild()) return '()';
  const first = cursor.name;
  if (first === 'ParenL') {
    cursor.nextSibling();
    if (cursor.name === 'QueryBody') {
      const inner = formatQueryBody(cursor, {...ctx, depth: ctx.depth + 1});
      cursor.parent();
      return '(\n' + inner + indent(ctx) + ')';
    }
    if (cursor.name === 'ExpressionList') {
      const list = formatExpressionList(cursor, ctx);
      cursor.parent();
      return '(' + list + ')';
    }
    // empty parens
    cursor.parent();
    return '()';
  }
  cursor.parent();
  return uppercaseKeywords(src(cursor, ctx));
}

function formatCaseExpr(cursor, ctx) {
  return uppercaseKeywords(src(cursor, ctx));
}

function formatCastExpr(cursor, ctx) {
  return uppercaseKeywords(src(cursor, ctx));
}

function formatExistsExpr(cursor, ctx) {
  return uppercaseKeywords(src(cursor, ctx));
}
