// Copyright (C) 2026 The Android Open Source Project
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

import {assertUnreachable} from '../../base/assert';
import {fnvHash} from './graph_utils';
import {
  type AggregateOp,
  type DistinctOp,
  type DropOp,
  type ExtendOp,
  type ExtractArgOp,
  type FilterLogicalOp,
  formatExtractArgExpr,
  formatFilterConditions,
  type GraphTreeOp,
  type IntervalFillGapsOp,
  type IntervalFlattenOp,
  type IntervalIntersectOp,
  type JoinOp,
  type LimitOp,
  type LogicalNode,
  type LogicalPlan,
  type PassthroughOp,
  type ProjectOp,
  type RawSqlOp,
  type RenameOp,
  type SetOp,
  type SortOp,
  type TimeRangeOp,
  type TreeAccumulateOp,
  type UnionOp,
} from './logical_ir';

// Content-addressable physical IR entry: the hash IS the CTE/table name
// (`_qb_<8hex>`). SQL in each entry references deps by their hash directly.
export interface IrEntry {
  // Content+dependency hash, also used as CTE/table name: _qb_<8hex>.
  readonly hash: string;
  // The query for this statement. References deps by hash and FROM tables by name.
  readonly sql: string;
  // The hashes of IR entries this depends on.
  readonly deps: readonly string[];
  // Module include keys required by this entry.
  readonly includes: readonly string[];
  // The node IDs that were folded into this entry.
  readonly nodeIds: readonly string[];
}

export function computeIrHash(
  sql: string,
  depHashes: readonly string[],
): string {
  const sorted = [...depHashes].sort();
  return `_qb_${fnvHash(sql + sorted.join(','))}`;
}

// --- Standard SQL statement folding ---

interface SqlStatement {
  distinct?: boolean;
  columns: string;
  from: string;
  where?: string;
  groupBy?: string;
  orderBy?: string;
  limit?: number;
  offset?: number;
}

type FoldableOp =
  | PassthroughOp
  | FilterLogicalOp
  | ProjectOp
  | ExtendOp
  | ExtractArgOp
  | DistinctOp
  | AggregateOp
  | SortOp
  | LimitOp;

// Split column list on top-level commas only (not inside parentheses).
function splitTopLevelCommas(s: string): string[] {
  const parts: string[] = [];
  let depth = 0;
  let start = 0;
  for (let i = 0; i < s.length; i++) {
    if (s[i] === '(') depth++;
    else if (s[i] === ')') depth--;
    else if (s[i] === ',' && depth === 0) {
      parts.push(s.slice(start, i).trim());
      start = i + 1;
    }
  }
  parts.push(s.slice(start).trim());
  return parts;
}

function formatColumns(columns: string): string {
  if (columns === '*') return columns;
  const cols = splitTopLevelCommas(columns);
  if (cols.length <= 1) return columns;
  return '\n  ' + cols.join(',\n  ');
}

function statementToSql(s: SqlStatement): string {
  const selectKw = s.distinct ? 'SELECT DISTINCT' : 'SELECT';
  const parts = [`${selectKw} ${formatColumns(s.columns)}`, `FROM ${s.from}`];
  if (s.where !== undefined && s.where !== '') parts.push(`WHERE ${s.where}`);
  if (s.groupBy !== undefined && s.groupBy !== '') {
    parts.push(`GROUP BY ${s.groupBy}`);
  }
  if (s.orderBy !== undefined && s.orderBy !== '') {
    parts.push(`ORDER BY ${s.orderBy}`);
  }
  if (s.limit !== undefined) {
    parts.push(
      s.offset !== undefined && s.offset > 0
        ? `LIMIT ${s.limit} OFFSET ${s.offset}`
        : `LIMIT ${s.limit}`,
    );
  }
  return parts.join('\n');
}

function tryFoldOp(stmt: SqlStatement, op: FoldableOp): boolean {
  switch (op.kind) {
    case 'passthrough':
      return true;

    case 'filter': {
      if (
        stmt.distinct !== undefined ||
        stmt.groupBy !== undefined ||
        stmt.orderBy !== undefined ||
        stmt.limit !== undefined
      ) {
        return false;
      }
      const expr = formatFilterConditions(op.conditions, op.conjunction);
      if (expr !== '') {
        stmt.where =
          stmt.where !== undefined && stmt.where !== ''
            ? `(${stmt.where}) AND (${expr})`
            : expr;
      }
      return true;
    }

    case 'project': {
      if (stmt.columns !== '*' || stmt.distinct !== undefined) return false;
      const entryParts = op.columns.map((c) =>
        c.alias !== undefined && c.alias !== ''
          ? `${c.column} AS ${c.alias}`
          : c.column,
      );
      const exprParts = op.expressions.map(
        (e) => `${e.expression} AS ${e.alias}`,
      );
      if (entryParts.length === 0) {
        if (exprParts.length > 0) {
          stmt.columns = `*, ${exprParts.join(', ')}`;
        }
        return true;
      }
      stmt.columns = [...entryParts, ...exprParts].join(', ');
      return true;
    }

    case 'extend': {
      if (stmt.groupBy !== undefined || stmt.distinct !== undefined) {
        return false;
      }
      const exprParts = op.expressions.map(
        (e) => `${e.expression} AS ${e.alias}`,
      );
      if (exprParts.length > 0) {
        stmt.columns = [stmt.columns, ...exprParts].join(', ');
      }
      return true;
    }

    case 'extract_arg': {
      if (stmt.groupBy !== undefined || stmt.distinct !== undefined) {
        return false;
      }
      const exprParts = op.extractions.map(
        (e) =>
          `${formatExtractArgExpr(op.argSetIdCol, e.argName)} AS ${e.alias}`,
      );
      if (exprParts.length > 0) {
        stmt.columns = [stmt.columns, ...exprParts].join(', ');
      }
      return true;
    }

    case 'distinct': {
      if (
        stmt.distinct !== undefined ||
        stmt.orderBy !== undefined ||
        stmt.limit !== undefined
      ) {
        return false;
      }
      stmt.distinct = true;
      return true;
    }

    case 'aggregate': {
      if (
        stmt.columns !== '*' ||
        stmt.distinct !== undefined ||
        stmt.groupBy !== undefined ||
        stmt.orderBy !== undefined ||
        stmt.limit !== undefined
      ) {
        return false;
      }
      const aggExprs = op.aggregations.map(
        (a) => `${a.func}(${a.column}) AS ${a.alias}`,
      );
      const selectParts = [...op.groupColumns, ...aggExprs];
      if (selectParts.length > 0) stmt.columns = selectParts.join(', ');
      if (op.groupColumns.length > 0) {
        stmt.groupBy = op.groupColumns.join(', ');
      }
      return true;
    }

    case 'sort': {
      if (stmt.orderBy !== undefined || stmt.limit !== undefined) return false;
      const orderBy = op.keys.map((k) => `${k.column} ${k.order}`).join(', ');
      if (orderBy !== '') {
        stmt.orderBy = orderBy;
      }
      return true;
    }

    case 'limit': {
      if (stmt.limit !== undefined) return false;
      stmt.limit = op.count;
      if (op.offset !== undefined && op.offset > 0) {
        stmt.offset = op.offset;
      }
      return true;
    }

    default:
      return assertUnreachable(op);
  }
}

// --- Standalone SQL emitters ---

function emitTimeRangeSql(op: TimeRangeOp): string {
  return `SELECT 0 AS id, ${op.ts} AS ts, ${op.dur} AS dur`;
}

function emitDropSql(op: DropOp, ref: string): string {
  const kept = op.remainingColumns ?? [];
  if (op.dropColumns.length === 0 || kept.length === 0) {
    return `SELECT *\nFROM ${ref}`;
  }
  const selectClause = kept.length > 1 ? '\n  ' + kept.join(',\n  ') : kept[0];
  return `SELECT ${selectClause}\nFROM ${ref}`;
}

function emitRenameSql(op: RenameOp, ref: string): string {
  const cols = op.inputColumns ?? [];
  if (op.renames.length === 0 || cols.length === 0) {
    return `SELECT *\nFROM ${ref}`;
  }
  const renameMap = new Map(op.renames.map((r) => [r.from, r.to]));
  const projected = cols.map((col) => {
    const target = renameMap.get(col);
    return target !== undefined && target !== col ? `${col} AS ${target}` : col;
  });
  const selectClause =
    projected.length > 1 ? '\n  ' + projected.join(',\n  ') : projected[0];
  return `SELECT ${selectClause}\nFROM ${ref}`;
}

function emitSetSql(op: SetOp, ref: string): string {
  if (op.assignments.length === 0) {
    return `SELECT *\nFROM ${ref}`;
  }
  const assignMap = new Map(
    op.assignments.map((a) => [a.column, a.expression]),
  );
  const cols = op.inputColumns ?? [];
  if (cols.length > 0) {
    const projected = cols.map((col) => {
      const expr = assignMap.get(col);
      return expr !== undefined ? `${expr} AS ${col}` : col;
    });
    const selectClause =
      projected.length > 1 ? '\n  ' + projected.join(',\n  ') : projected[0];
    return `SELECT ${selectClause}\nFROM ${ref}`;
  }
  const appended = op.assignments
    .map((a) => `${a.expression} AS ${a.column}`)
    .join(', ');
  return `SELECT *, ${appended}\nFROM ${ref}`;
}

function emitIntervalFlattenSql(op: IntervalFlattenOp, ref: string): string {
  const uniquePer = [...new Set(op.partitionColumns)];
  const perPrefix = uniquePer.length > 0 ? `${uniquePer.join(', ')}, ` : '';
  const partBy =
    uniquePer.length > 0 ? `PARTITION BY ${uniquePer.join(', ')} ` : '';
  const hasMinMax = op.aggregations.some(
    (a) => a.func === 'MIN' || a.func === 'MAX',
  );

  if (!hasMinMax) {
    const sumAggs = op.aggregations
      .map((a, idx) => ({a, idx}))
      .filter((x) => x.a.func === 'SUM');

    const startExtras = sumAggs.map(
      ({a, idx}) =>
        `, IIF(${a.column} IS NOT NULL, ${a.column}, 0) AS _s${idx}, IIF(${a.column} IS NOT NULL, 1, 0) AS _c${idx}`,
    );
    const endExtras = sumAggs.map(
      ({a, idx}) =>
        `, -IIF(${a.column} IS NOT NULL, ${a.column}, 0) AS _s${idx}, -IIF(${a.column} IS NOT NULL, 1, 0) AS _c${idx}`,
    );
    const mergedExtras = sumAggs.map(
      ({idx}) => `, SUM(_s${idx}) AS _s${idx}, SUM(_c${idx}) AS _c${idx}`,
    );
    const windowSpec = `${partBy}ORDER BY ts ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW`;
    const segExtras = sumAggs.map(
      ({idx}) =>
        `,\n      SUM(_s${idx}) OVER (${windowSpec}) AS _rs${idx},\n      SUM(_c${idx}) OVER (${windowSpec}) AS _rc${idx}`,
    );

    const outCols: string[] = ['ts', 'dur', ...op.partitionColumns];
    for (const [idx, a] of op.aggregations.entries()) {
      if (a.func === 'COUNT') {
        outCols.push(`_active AS ${a.alias}`);
      } else {
        outCols.push(`IIF(_rc${idx} > 0, _rs${idx}, NULL) AS ${a.alias}`);
      }
    }

    return [
      'WITH',
      `  _src AS (\n    SELECT * FROM ${ref} WHERE ts IS NOT NULL AND dur > 0\n  ),`,
      `  _events AS (\n    SELECT ${perPrefix}ts, 1 AS _delta${startExtras.join('')} FROM _src\n    UNION ALL\n    SELECT ${perPrefix}ts + dur AS ts, -1 AS _delta${endExtras.join('')} FROM _src\n  ),`,
      `  _merged AS (\n    SELECT ${perPrefix}ts, SUM(_delta) AS _delta${mergedExtras.join('')}\n    FROM _events\n    GROUP BY ${perPrefix}ts\n  ),`,
      `  _segs AS (\n    SELECT\n      ts,\n      LEAD(ts) OVER (${partBy}ORDER BY ts) - ts AS dur,\n      ${perPrefix}SUM(_delta) OVER (${windowSpec}) AS _active${segExtras.join('')}\n    FROM _merged\n  )`,
      `SELECT\n  ${outCols.join(',\n  ')}\nFROM _segs\nWHERE _active > 0 AND dur > 0`,
    ].join('\n');
  }

  const windowSpec = `${partBy}ORDER BY ts ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW`;
  const joinConds = [
    ...uniquePer.map((p) => `s.${p} IS r.${p}`),
    'r.ts <= s.ts',
    'r.ts + r.dur >= s.ts + s.dur',
  ].join(' AND ');
  const outCols: string[] = [
    's.ts',
    's.dur',
    ...op.partitionColumns.map((p) => `s.${p}`),
    ...op.aggregations.map((a) =>
      a.func === 'COUNT'
        ? `COUNT(*) AS ${a.alias}`
        : `${a.func}(r.${a.column}) AS ${a.alias}`,
    ),
  ];

  return [
    'WITH',
    `  _src AS (\n    SELECT * FROM ${ref} WHERE ts IS NOT NULL AND dur > 0\n  ),`,
    `  _events AS (\n    SELECT ${perPrefix}ts, 1 AS _delta FROM _src\n    UNION ALL\n    SELECT ${perPrefix}ts + dur AS ts, -1 AS _delta FROM _src\n  ),`,
    `  _merged AS (\n    SELECT ${perPrefix}ts, SUM(_delta) AS _delta\n    FROM _events\n    GROUP BY ${perPrefix}ts\n  ),`,
    `  _segs AS (\n    SELECT\n      ROW_NUMBER() OVER () AS _seg_id,\n      ts,\n      LEAD(ts) OVER (${partBy}ORDER BY ts) - ts AS dur,\n      ${perPrefix}SUM(_delta) OVER (${windowSpec}) AS _active\n    FROM _merged\n  )`,
    `SELECT\n  ${outCols.join(',\n  ')}\nFROM _segs AS s\nJOIN _src AS r ON ${joinConds}\nWHERE s._active > 0 AND s.dur > 0\nGROUP BY s._seg_id`,
  ].join('\n');
}

function emitIntervalFillGapsSql(
  op: IntervalFillGapsOp,
  fgRef: string,
  bgRef: string,
): string {
  const uniquePer = [...new Set(op.partitionColumns)];
  const perPrefix = uniquePer.length > 0 ? `${uniquePer.join(', ')}, ` : '';
  const partBy =
    uniquePer.length > 0 ? `PARTITION BY ${uniquePer.join(', ')} ` : '';
  const partOnly =
    uniquePer.length > 0 ? `PARTITION BY ${uniquePer.join(', ')}` : '';

  const bgCols = op.backgroundColumns ?? [];
  const bgColSet = new Set(bgCols);
  const bgHasPer =
    uniquePer.length === 0 ||
    (op.backgroundColumns !== undefined &&
      uniquePer.every((p) => bgColSet.has(p)));

  const ctes: string[] = [];
  if (bgHasPer) {
    ctes.push(
      `  _bg AS (\n    SELECT * FROM ${bgRef} WHERE ts IS NOT NULL AND dur > 0\n  )`,
    );
  } else {
    const bgExtra = bgCols.filter(
      (c) => c !== 'ts' && c !== 'dur' && !uniquePer.includes(c),
    );
    const bgProj = [
      ...uniquePer.map((p) => `l.${p}`),
      'b.ts',
      'b.dur',
      ...bgExtra.map((c) => `b.${c}`),
    ].join(', ');
    ctes.push(
      `  _lanes AS (\n    SELECT DISTINCT ${uniquePer.join(', ')} FROM ${fgRef}\n  )`,
      `  _bg AS (\n    SELECT ${bgProj}\n    FROM _lanes AS l\n    CROSS JOIN (SELECT * FROM ${bgRef} WHERE ts IS NOT NULL AND dur > 0) AS b\n  )`,
    );
  }

  const maxInt64 = '9223372036854775807';
  const notExistsWhere =
    uniquePer.length > 0
      ? ` WHERE ${uniquePer.map((p) => `i.${p} IS b.${p}`).join(' AND ')}`
      : '';

  ctes.push(
    `  _fg_valid AS (\n    SELECT\n      ${perPrefix}ts AS _s,\n      IIF(dur = -1, ${maxInt64}, ts + dur) AS _e\n    FROM ${fgRef}\n    WHERE ts IS NOT NULL AND (dur > 0 OR dur = -1)\n  )`,
    `  _fg_max AS (\n    SELECT\n      *,\n      MAX(_e) OVER (${partBy}ORDER BY _s ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING) AS _prev_max\n    FROM _fg_valid\n  )`,
    `  _fg_grp AS (\n    SELECT\n      *,\n      SUM(IIF(_prev_max IS NULL OR _s > _prev_max, 1, 0)) OVER (${partBy}ORDER BY _s ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS _isl\n    FROM _fg_max\n  )`,
    `  _islands AS (\n    SELECT\n      ${perPrefix}MIN(_s) AS _s,\n      MAX(_e) AS _e,\n      ROW_NUMBER() OVER (${partBy}ORDER BY MIN(_s)) AS _rn,\n      COUNT(*) OVER (${partOnly}) AS _cnt\n    FROM _fg_grp\n    GROUP BY ${perPrefix}_isl\n  )`,
    `  _isl_prev AS (\n    SELECT\n      *,\n      LAG(_e) OVER (${partBy}ORDER BY _s) AS _prev_e\n    FROM _islands\n  )`,
    [
      '  _gaps AS (',
      `    SELECT ${perPrefix}COALESCE(_prev_e, 0) AS _gs, _s AS _ge FROM _isl_prev WHERE _s > COALESCE(_prev_e, 0)`,
      '    UNION ALL',
      `    SELECT ${perPrefix}_e AS _gs, ${maxInt64} AS _ge FROM _isl_prev WHERE _rn = _cnt AND _e < ${maxInt64}`,
      '    UNION ALL',
      `    SELECT DISTINCT ${uniquePer.map((p) => `b.${p}, `).join('')}0 AS _gs, ${maxInt64} AS _ge FROM _bg AS b WHERE NOT EXISTS (SELECT 1 FROM _islands AS i${notExistsWhere})`,
      '  )',
    ].join('\n'),
  );

  const outSchema = op.inputColumns ?? ['ts', 'dur', ...uniquePer];
  const availableOnBg = new Set([...bgCols, ...uniquePer]);
  const fillerProj = outSchema.map((col) => {
    if (col === 'ts') return 'MAX(g._gs, b.ts) AS ts';
    if (col === 'dur') {
      return 'MIN(g._ge, b.ts + b.dur) - MAX(g._gs, b.ts) AS dur';
    }
    if (availableOnBg.has(col)) return `b.${col} AS ${col}`;
    return `NULL AS ${col}`;
  });

  const joinCond =
    uniquePer.length > 0
      ? `ON ${uniquePer.map((p) => `b.${p} IS g.${p}`).join(' AND ')}\n`
      : '';

  return [
    'WITH',
    ctes.join(',\n'),
    `SELECT *\nFROM ${fgRef}`,
    'UNION ALL',
    `SELECT\n  ${fillerProj.join(',\n  ')}\nFROM _bg AS b\nJOIN _gaps AS g ${joinCond}WHERE MIN(g._ge, b.ts + b.dur) > MAX(g._gs, b.ts)`,
  ].join('\n');
}

function emitTreeAccumulateSql(op: TreeAccumulateOp, ref: string): string {
  if (op.aggregations.length === 0) {
    return `SELECT *\nFROM ${ref}`;
  }
  const keyExpr = op.direction === 'UP' ? 'c._anchor' : 'c._member';
  const valJoinKey = op.direction === 'UP' ? 'c._member' : 'c._anchor';
  const aggExprs = op.aggregations.map((a) =>
    a.func === 'COUNT'
      ? `COUNT(*) AS ${a.alias}`
      : `${a.func}(v.${a.column}) AS ${a.alias}`,
  );
  const outerCols = op.aggregations.map((a) => `_agg.${a.alias}`);

  return [
    'WITH RECURSIVE',
    `  _closure(_anchor, _member) AS (\n    SELECT ${op.idColumn}, ${op.idColumn} FROM ${ref}\n    UNION ALL\n    SELECT c._anchor, child.${op.idColumn}\n    FROM _closure AS c\n    JOIN ${ref} AS child ON child.${op.parentIdColumn} = c._member\n  ),`,
    `  _agg AS (\n    SELECT\n      ${keyExpr} AS _key_id,\n      ${aggExprs.join(',\n      ')}\n    FROM _closure AS c\n    JOIN ${ref} AS v ON v.${op.idColumn} = ${valJoinKey}\n    GROUP BY ${keyExpr}\n  )`,
    `SELECT\n  src.*,\n  ${outerCols.join(',\n  ')}\nFROM ${ref} AS src\nLEFT JOIN _agg ON src.${op.idColumn} = _agg._key_id`,
  ].join('\n');
}

function emitGraphTreeSql(
  op: GraphTreeOp,
  nodesRef: string,
  edgesRef: string,
  rootsRef: string,
): string {
  const carried =
    op.nodeColumns && op.nodeColumns.length > 0
      ? op.nodeColumns
          .filter((c) => c !== 'parent_id')
          .map((c) =>
            c === op.nodeIdColumn && op.nodeIdColumn !== 'id'
              ? `n.${c} AS id`
              : `n.${c}`,
          )
      : ['n.*'];

  const edgesSubq = `SELECT ${op.sourceColumn} AS source_node_id, ${op.destColumn} AS dest_node_id FROM ${edgesRef} WHERE ${op.sourceColumn} IS NOT NULL AND ${op.destColumn} IS NOT NULL`;
  const rootsSubq = `SELECT ${op.rootIdColumn} AS node_id FROM ${rootsRef} WHERE ${op.rootIdColumn} IS NOT NULL`;

  if (op.treeKind === 'BFS' || op.treeKind === 'DFS') {
    const macro =
      op.treeKind === 'BFS' ? 'graph_reachable_bfs!' : 'graph_reachable_dfs!';
    return [
      `SELECT\n  ${[...carried, '_tree.parent_node_id AS parent_id'].join(',\n  ')}`,
      `FROM ${nodesRef} AS n`,
      `JOIN ${macro}(\n  (${edgesSubq}),\n  (${rootsSubq})\n) AS _tree ON n.${op.nodeIdColumn} = _tree.node_id`,
    ].join('\n');
  }

  return [
    'WITH',
    `  _edges AS (\n    ${edgesSubq}\n  ),`,
    `  _roots AS (\n    ${rootsSubq}\n  ),`,
    '  _reach AS (\n    SELECT node_id FROM graph_reachable_dfs!(_edges, _roots)\n  ),',
    `  _super AS (\n    SELECT COALESCE(MAX(${op.nodeIdColumn}), 0) + 1 AS root_id FROM ${nodesRef}\n  ),`,
    '  _flow AS (\n    SELECT e.source_node_id, e.dest_node_id\n    FROM _edges AS e\n    JOIN _reach AS r ON e.source_node_id = r.node_id\n    UNION ALL\n    SELECT (SELECT root_id FROM _super) AS source_node_id, node_id AS dest_node_id\n    FROM _roots\n  ),',
    '  _dom AS (\n    SELECT\n      node_id,\n      IIF(dominator_node_id = (SELECT root_id FROM _super), NULL, dominator_node_id) AS parent_id\n    FROM graph_dominator_tree!(_flow, (SELECT root_id FROM _super))\n    WHERE node_id != (SELECT root_id FROM _super)\n  )',
    `SELECT\n  ${[...carried, '_dom.parent_id'].join(',\n  ')}\nFROM ${nodesRef} AS n\nJOIN _dom ON n.${op.nodeIdColumn} = _dom.node_id`,
  ].join('\n');
}

function emitJoinSql(op: JoinOp, leftRef: string, rightRef: string): string {
  const selectCols: [string, ...string[]] = ['l.*'];
  for (const a of op.rightColumns) {
    const expr = `r.${a.column}`;
    selectCols.push(a.alias !== a.column ? `${expr} AS ${a.alias}` : expr);
  }
  const selectClause =
    selectCols.length > 1 ? '\n  ' + selectCols.join(',\n  ') : selectCols[0];
  const joinKw = op.joinType === 'INNER' ? 'JOIN' : 'LEFT JOIN';
  const condition = `ON l.${op.leftColumn} = r.${op.rightColumn}`;
  return `SELECT ${selectClause}\nFROM ${leftRef} AS l\n${joinKw} ${rightRef} AS r ${condition}`;
}

function emitUnionSql(op: UnionOp, refs: readonly string[]): string {
  const kw = op.distinct ? 'UNION' : 'UNION ALL';
  return refs.map((r) => `SELECT *\nFROM ${r}`).join(`\n${kw}\n`);
}

function emitIntervalIntersectSql(
  op: IntervalIntersectOp,
  refs: readonly string[],
): string {
  const partitionClause =
    op.partitionColumns.length > 0 ? op.partitionColumns.join(', ') : '';
  const args = refs.map((ref) =>
    op.filterNegativeDur ? `(SELECT * FROM ${ref} WHERE dur >= 0)` : ref,
  );
  const n = op.inputs.length;
  const selectParts = ['ii.ts', 'ii.dur'];
  for (let i = 0; i < n; i++) {
    selectParts.push(`ii.id_${i}`);
  }
  for (const [i, inp] of op.inputs.entries()) {
    for (const col of inp.columns) {
      const expr = `t${i + 1}.${col.name}`;
      selectParts.push(
        col.alias !== col.name ? `${expr} AS ${col.alias}` : expr,
      );
    }
  }

  const hasSourceCols = op.inputs.some((inp) => inp.columns.length > 0);
  const argsStr = args.join(',\n   ');

  if (hasSourceCols) {
    const joins = refs
      .map((ref, i) => `JOIN ${ref} t${i + 1} ON ii.id_${i} = t${i + 1}.id`)
      .join('\n');
    return (
      `SELECT\n${selectParts.map((s) => `  ${s}`).join(',\n')}` +
      `\nFROM _interval_intersect!(\n  (${argsStr}),\n  (${partitionClause})\n) ii` +
      `\n${joins}`
    );
  }
  return `SELECT *\nFROM _interval_intersect!(\n  (${argsStr}),\n  (${partitionClause})\n)`;
}

function emitRawSql(
  op: RawSqlOp,
  resolveRef: (nodeId: string) => string,
): string {
  const withClauses = op.inputs.map(
    (inp) => `${inp.alias} AS (\n  SELECT * FROM ${resolveRef(inp.nodeId)}\n)`,
  );
  if (withClauses.length === 0) {
    return op.sql;
  }
  const trimmed = op.sql.trimStart();
  const withMatch = trimmed.match(/^with\s/i);
  const body = withMatch ? trimmed.slice(withMatch[0].length) : trimmed;
  const separator = withMatch ? ',\n' : '\n';
  return `WITH ${withClauses.join(',\n')}${separator}${body}`;
}

// --- Lower LogicalPlan -> Standard SQL IrEntry[] ---

export function lowerToSql(plan: LogicalPlan): IrEntry[] {
  const nodeById = new Map<string, LogicalNode>();
  for (const node of plan.nodes) {
    nodeById.set(node.nodeId, node);
  }

  const entries: IrEntry[] = [];
  const entryByHash = new Set<string>();
  const emittedAs = new Map<string, string>();
  const isScanNode = new Set<string>();
  const nodeIncludes = new Map<string, Set<string>>();

  let current: SqlStatement | undefined;
  let currentTailId: string | undefined;
  let currentDeps: string[] = [];
  let currentIncludes = new Set<string>();
  let currentNodeIds: string[] = [];

  function collectIncludes(inputIds: readonly string[]): Set<string> {
    const includes = new Set<string>();
    for (const id of inputIds) {
      const parentIncludes = nodeIncludes.get(id);
      if (parentIncludes) {
        for (const inc of parentIncludes) includes.add(inc);
      }
    }
    return includes;
  }

  function emitEntry(
    sql: string,
    deps: readonly string[],
    includes: ReadonlySet<string>,
    nodeIds: readonly string[],
  ): string {
    const hash = computeIrHash(sql, deps);
    if (!entryByHash.has(hash)) {
      entryByHash.add(hash);
      entries.push({
        hash,
        sql,
        deps: [...deps],
        includes: [...includes],
        nodeIds: [...nodeIds],
      });
    }
    return hash;
  }

  function emitCurrent(): void {
    if (current !== undefined && currentTailId !== undefined) {
      const sql = statementToSql(current);
      const hash = emitEntry(sql, currentDeps, currentIncludes, currentNodeIds);
      emittedAs.set(currentTailId, hash);
    }
    current = undefined;
    currentTailId = undefined;
    currentDeps = [];
    currentIncludes = new Set<string>();
    currentNodeIds = [];
  }

  function resolveRef(id: string): string {
    return emittedAs.get(id) ?? '';
  }

  function resolveDep(id: string): string | undefined {
    if (isScanNode.has(id)) return undefined;
    return emittedAs.get(id);
  }

  function resolveDeps(inputIds: readonly string[]): string[] {
    const seen = new Set<string>();
    const deps: string[] = [];
    for (const id of inputIds) {
      const dep = resolveDep(id);
      if (dep !== undefined && !seen.has(dep)) {
        seen.add(dep);
        deps.push(dep);
      }
    }
    return deps;
  }

  function emitStandalone(
    nodeId: string,
    inputIds: readonly string[],
    buildSql: () => string,
    extraIncludes?: readonly string[],
  ): boolean {
    emitCurrent();
    if (inputIds.some((id) => resolveRef(id) === '')) return false;
    const sql = buildSql();
    const deps = resolveDeps(inputIds);
    const includes = collectIncludes(inputIds);
    if (extraIncludes) {
      for (const inc of extraIncludes) includes.add(inc);
    }
    const hash = emitEntry(sql, deps, includes, [nodeId]);
    nodeIncludes.set(nodeId, includes);
    emittedAs.set(nodeId, hash);
    return true;
  }

  function emitFoldable(nodeId: string, op: FoldableOp): boolean {
    const inputIsCurrentTail = op.input === currentTailId;
    const inputSingleRef = (nodeById.get(op.input)?.refCount ?? 0) <= 1;
    const inherited = collectIncludes([op.input]);

    if (
      inputIsCurrentTail &&
      inputSingleRef &&
      current !== undefined &&
      tryFoldOp(current, op)
    ) {
      currentTailId = nodeId;
      currentNodeIds.push(nodeId);
      for (const inc of inherited) currentIncludes.add(inc);
      nodeIncludes.set(nodeId, currentIncludes);
      return true;
    }

    emitCurrent();
    const inputRef = resolveRef(op.input);
    if (inputRef === '') return false;
    current = {columns: '*', from: inputRef};
    tryFoldOp(current, op);
    currentTailId = nodeId;
    currentNodeIds = [nodeId];
    currentIncludes = inherited;
    const dep = resolveDep(op.input);
    if (dep !== undefined) currentDeps.push(dep);
    nodeIncludes.set(nodeId, currentIncludes);
    return true;
  }

  for (const node of plan.nodes) {
    const {nodeId, op} = node;
    switch (op.kind) {
      case 'scan': {
        emitCurrent();
        emittedAs.set(nodeId, op.table);
        isScanNode.add(nodeId);
        if (op.includes && op.includes.length > 0) {
          nodeIncludes.set(nodeId, new Set(op.includes));
        }
        break;
      }

      case 'time_range': {
        emitStandalone(nodeId, [], () => emitTimeRangeSql(op));
        break;
      }

      case 'drop': {
        if (
          !emitStandalone(nodeId, [op.input], () =>
            emitDropSql(op, resolveRef(op.input)),
          )
        ) {
          return [];
        }
        break;
      }

      case 'rename': {
        if (
          !emitStandalone(nodeId, [op.input], () =>
            emitRenameSql(op, resolveRef(op.input)),
          )
        ) {
          return [];
        }
        break;
      }

      case 'set': {
        if (
          !emitStandalone(nodeId, [op.input], () =>
            emitSetSql(op, resolveRef(op.input)),
          )
        ) {
          return [];
        }
        break;
      }

      case 'join': {
        if (
          !emitStandalone(nodeId, [op.left, op.right], () =>
            emitJoinSql(op, resolveRef(op.left), resolveRef(op.right)),
          )
        ) {
          return [];
        }
        break;
      }

      case 'union': {
        if (op.inputs.length === 0) return [];
        if (
          !emitStandalone(nodeId, op.inputs, () =>
            emitUnionSql(op, op.inputs.map(resolveRef)),
          )
        ) {
          return [];
        }
        break;
      }

      case 'interval_intersect': {
        const inputIds = op.inputs.map((inp) => inp.nodeId);
        if (inputIds.length === 0) return [];
        if (
          !emitStandalone(
            nodeId,
            inputIds,
            () => emitIntervalIntersectSql(op, inputIds.map(resolveRef)),
            ['intervals.intersect'],
          )
        ) {
          return [];
        }
        break;
      }

      case 'interval_flatten': {
        if (
          !emitStandalone(nodeId, [op.input], () =>
            emitIntervalFlattenSql(op, resolveRef(op.input)),
          )
        ) {
          return [];
        }
        break;
      }

      case 'interval_fill_gaps': {
        if (
          !emitStandalone(nodeId, [op.input, op.background], () =>
            emitIntervalFillGapsSql(
              op,
              resolveRef(op.input),
              resolveRef(op.background),
            ),
          )
        ) {
          return [];
        }
        break;
      }

      case 'tree_accumulate': {
        if (
          !emitStandalone(nodeId, [op.input], () =>
            emitTreeAccumulateSql(op, resolveRef(op.input)),
          )
        ) {
          return [];
        }
        break;
      }

      case 'graph_tree': {
        const graphIncludes =
          op.treeKind === 'DOMINATOR'
            ? ['graphs.search', 'graphs.dominator_tree']
            : ['graphs.search'];
        if (
          !emitStandalone(
            nodeId,
            [op.nodes, op.edges, op.roots],
            () =>
              emitGraphTreeSql(
                op,
                resolveRef(op.nodes),
                resolveRef(op.edges),
                resolveRef(op.roots),
              ),
            graphIncludes,
          )
        ) {
          return [];
        }
        break;
      }

      case 'raw_sql': {
        const inputIds = op.inputs.map((inp) => inp.nodeId);
        if (
          !emitStandalone(nodeId, inputIds, () => emitRawSql(op, resolveRef))
        ) {
          return [];
        }
        break;
      }

      case 'passthrough':
      case 'filter':
      case 'project':
      case 'extend':
      case 'extract_arg':
      case 'distinct':
      case 'aggregate':
      case 'sort':
      case 'limit': {
        if (!emitFoldable(nodeId, op)) return [];
        break;
      }

      default:
        assertUnreachable(op);
    }
  }

  emitCurrent();

  // If the target node itself is a scan (`FROM`), emit `SELECT * FROM <table>`.
  const targetNode = nodeById.get(plan.targetNodeId);
  if (targetNode?.op.kind === 'scan') {
    const includes = nodeIncludes.get(targetNode.nodeId) ?? new Set<string>();
    emitEntry(`SELECT *\nFROM ${targetNode.op.table}`, [], includes, [
      targetNode.nodeId,
    ]);
  }

  return entries;
}

// --- Display SQL ---

// Format Standard SQL IR entries as a single query using `WITH` CTEs.
export function buildDisplaySql(
  entries: readonly IrEntry[],
): string | undefined {
  if (entries.length === 0) return undefined;

  const allIncludes = new Set<string>();
  for (const e of entries) {
    for (const inc of e.includes) allIncludes.add(inc);
  }

  const lastEntry = entries[entries.length - 1];
  if (!lastEntry) return undefined;
  const cteEntries = entries.slice(0, -1);

  const parts: string[] = [];

  for (const inc of allIncludes) {
    parts.push(`INCLUDE PERFETTO MODULE ${inc};`);
  }

  const nameMap = new Map<string, string>();
  for (const [i, cteEntry] of cteEntries.entries()) {
    nameMap.set(cteEntry.hash, `step_${i + 1}`);
  }

  function renameSql(sql: string): string {
    let result = sql;
    for (const [hash, name] of nameMap) {
      result = result.replaceAll(hash, name);
    }
    return result;
  }

  if (cteEntries.length === 0) {
    parts.push(lastEntry.sql);
  } else {
    const cteParts = cteEntries.map((e) => {
      const indented = renameSql(e.sql).split('\n').join('\n  ');
      return `${nameMap.get(e.hash)} AS (\n  ${indented}\n)`;
    });
    parts.push(`WITH\n${cteParts.join(',\n')}\n${renameSql(lastEntry.sql)}`);
  }

  return parts.join('\n');
}
