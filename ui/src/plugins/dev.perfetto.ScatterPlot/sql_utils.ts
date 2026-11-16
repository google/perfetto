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

import {stripTrailingSemicolons} from '../../trace_processor/sql_utils';
import type {DataBounds, PlotSpec, SourceSpec} from './types';

export function quoteIdentifier(name: string): string {
  return `"${name.replace(/"/g, '""')}"`;
}

export function formatNumber(n: number): string {
  if (!Number.isFinite(n)) {
    throw new Error(`Non-finite number cannot be formatted for SQL: ${n}`);
  }
  return String(n);
}

export function sourceSubquery(spec: SourceSpec): string {
  if (spec.kind === 'table') {
    if (spec.table.trim().length === 0) {
      throw new Error('Table name cannot be empty');
    }
    return quoteIdentifier(spec.table);
  }
  const sql = stripTrailingSemicolons(spec.sql).trim();
  if (sql.length === 0) {
    throw new Error('SQL query cannot be empty');
  }
  return `(${sql})`;
}

export function includeModuleSql(module: string): string {
  const trimmed = module.trim();
  if (trimmed.length === 0) {
    throw new Error('Module name cannot be empty');
  }
  return `INCLUDE PERFETTO MODULE ${trimmed};`;
}

export function scatterMipmapSubqueryArgs(
  spec: SourceSpec,
  plot: PlotSpec,
  colorCol?: string,
): string {
  const fromClause = sourceSubquery(spec);
  const xCol = quoteIdentifier(plot.x);
  const yCol = quoteIdentifier(plot.y);
  const cExpr =
    colorCol !== undefined && colorCol !== ''
      ? quoteIdentifier(colorCol)
      : 'NULL';
  return `((SELECT NULL AS id, ${xCol} AS x, ${yCol} AS y, ${cExpr} AS c FROM ${fromClause}))`;
}

export function scatterMipmapTableArgs(
  tableName: string,
  idCol: string,
  xCol: string,
  yCol: string,
  cCol?: string,
  topN?: number,
): string {
  const idArg = idCol ? quoteIdentifier(idCol) : "''";
  const cArg =
    cCol !== undefined && cCol !== '' ? `, ${quoteIdentifier(cCol)}` : '';
  const catArg = topN !== undefined ? `, ${formatNumber(topN)}` : '';
  return `(${quoteIdentifier(tableName)}, ${idArg}, ${quoteIdentifier(xCol)}, ${quoteIdentifier(yCol)}${cArg}${catArg})`;
}

export function scatterMipmapSubquerySql(
  vtTable: string,
  spec: SourceSpec,
  plot: PlotSpec,
  colorCol?: string,
): string {
  return `CREATE VIRTUAL TABLE ${quoteIdentifier(vtTable)} USING __intrinsic_scatter_mipmap${scatterMipmapSubqueryArgs(spec, plot, colorCol)};`;
}

export function scatterMipmapVirtualTableSql(
  vtTable: string,
  tableName: string,
  idCol: string,
  xCol: string,
  yCol: string,
  cCol?: string,
  topN?: number,
): string {
  return `CREATE VIRTUAL TABLE ${quoteIdentifier(vtTable)} USING __intrinsic_scatter_mipmap${scatterMipmapTableArgs(tableName, idCol, xCol, yCol, cCol, topN)};`;
}

export function vtableStatsQuerySql(vtTable: string): string {
  return (
    `SELECT out_count, out_x_min, out_x_max, out_y_min, out_y_max, out_c_min, out_c_max ` +
    `FROM ${quoteIdentifier(vtTable)}(NULL, NULL, NULL, NULL, 1, 1) LIMIT 1;`
  );
}

export function vtableCategoriesQuerySql(vtTable: string): string {
  return (
    `SELECT out_cat_idx, out_cat_value, out_cat_count ` +
    `FROM ${quoteIdentifier(vtTable)}(NULL, NULL, NULL, NULL, 0, 0);`
  );
}

export function viewportQuerySql(
  vtTable: string,
  req: {
    readonly xMin: number;
    readonly xMax: number;
    readonly yMin: number;
    readonly yMax: number;
    readonly cols: number;
    readonly rows: number;
    readonly hidden?: ReadonlyArray<number>;
  },
): string {
  const hidden = (req.hidden ?? []).filter((i) => Number.isInteger(i));
  const hiddenArg = hidden.length > 0 ? `, '${hidden.join(',')}'` : '';
  return (
    `SELECT id, x, y, c, count FROM ${quoteIdentifier(vtTable)}(` +
    `${formatNumber(req.xMin)}, ${formatNumber(req.xMax)}, ` +
    `${formatNumber(req.yMin)}, ${formatNumber(req.yMax)}, ` +
    `${formatNumber(Math.round(req.cols))}, ${formatNumber(Math.round(req.rows))}` +
    `${hiddenArg});`
  );
}

export function rowFetchSql(
  tableName: string,
  id: number,
  idCol = 'id',
): string {
  return `SELECT * FROM ${quoteIdentifier(tableName)} WHERE ${quoteIdentifier(idCol)} = ${formatNumber(id)};`;
}

export function rowFetchByOffsetSql(
  fromSource: string,
  offset: number,
): string {
  return `SELECT * FROM ${fromSource} LIMIT 1 OFFSET ${formatNumber(offset)};`;
}

function rectWhereClause(plot: PlotSpec, rect: DataBounds): string {
  const xCol = quoteIdentifier(plot.x);
  const yCol = quoteIdentifier(plot.y);
  const xMin = formatNumber(Math.min(rect.xMin, rect.xMax));
  const xMax = formatNumber(Math.max(rect.xMin, rect.xMax));
  const yMin = formatNumber(Math.min(rect.yMin, rect.yMax));
  const yMax = formatNumber(Math.max(rect.yMin, rect.yMax));
  return `WHERE ${xCol} BETWEEN ${xMin} AND ${xMax} AND ${yCol} BETWEEN ${yMin} AND ${yMax}`;
}

export function countInRectSql(
  fromSource: string,
  plot: PlotSpec,
  rect: DataBounds,
): string {
  return `SELECT COUNT(*) AS count FROM ${fromSource} ${rectWhereClause(plot, rect)};`;
}

export function sqlForRect(
  source: SourceSpec,
  plot: PlotSpec,
  rect: DataBounds,
): string {
  return `SELECT * FROM ${sourceSubquery(source)} ${rectWhereClause(plot, rect)};`;
}
