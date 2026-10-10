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

import {AsyncDisposableStack} from '../../base/disposable_stack';
import type {Engine} from '../../trace_processor/engine';
import {
  NUM,
  NUM_NULL,
  type QueryResult,
  type SqlValue,
} from '../../trace_processor/query_result';
import {
  createView,
  createVirtualTable,
  fromNumNull,
} from '../../trace_processor/sql_utils';
import {
  type ColumnKind,
  type ColumnInfo,
  type ColorMode,
  type DataBounds,
  NULL_CATEGORY,
  type PlotSpec,
  type PointColumns,
  type SourceSpec,
  TOP_N_CATEGORIES,
} from './types';
import {
  countInRectSql,
  includeModuleSql,
  rowFetchByOffsetSql,
  rowFetchSql,
  scatterMipmapSubqueryArgs,
  scatterMipmapTableArgs,
  sourceSubquery,
  sqlForRect,
  viewportQuerySql,
  vtableCategoriesQuerySql,
  vtableStatsQuerySql,
} from './sql_utils';

export interface ViewportRequest {
  readonly xMin: number;
  readonly xMax: number;
  readonly yMin: number;
  readonly yMax: number;
  readonly cols: number;
  readonly rows: number;
  /**
   * Category indexes to exclude (NULL_CATEGORY for NULL). Excluded points are
   * neither counted nor returned as cell representatives.
   */
  readonly hidden?: ReadonlyArray<number>;
}

export interface PreparedSource {
  readonly count: number;
  readonly bounds: DataBounds;
  readonly colorMode: ColorMode;
  fetchViewport(
    req: ViewportRequest,
  ): Promise<PointColumns & {readonly count: Float64Array}>;
  fetchRow(
    id: number,
  ): Promise<ReadonlyArray<{readonly name: string; readonly value: SqlValue}>>;
  countInRect(rect: DataBounds): Promise<number>;
  sqlForRect(rect: DataBounds): string;
}

function decodeViewportColumns(
  result: QueryResult,
  colorMode: ColorMode,
): PointColumns & {readonly count: Float64Array} {
  const nullValue = colorMode.kind === 'categorical' ? -1 : NaN;

  if (typeof result.decodeColumns === 'function') {
    const decoded = result.decodeColumns({
      id: NUM,
      x: NUM,
      y: NUM,
      c: NUM_NULL,
      count: NUM,
    });
    const n = decoded.x.length;
    const color = new Float32Array(n);
    const c = decoded.c;
    for (let i = 0; i < n; ++i) {
      color[i] = fromNumNull(c[i]) ?? nullValue;
    }
    return {
      x: decoded.x,
      y: decoded.y,
      row: decoded.id,
      color,
      count: decoded.count,
    };
  }

  const n = result.numRows();
  const x = new Float64Array(n);
  const y = new Float64Array(n);
  const row = new Float64Array(n);
  const color = new Float32Array(n);
  const count = new Float64Array(n);
  let i = 0;
  for (
    const it = result.iter({
      id: NUM,
      x: NUM,
      y: NUM,
      c: NUM_NULL,
      count: NUM,
    });
    it.valid();
    it.next(), i++
  ) {
    row[i] = it.id;
    x[i] = it.x;
    y[i] = it.y;
    color[i] = fromNumNull(it.c) ?? nullValue;
    count[i] = it.count;
  }
  return {x, y, row, color, count};
}

class PreparedSourceImpl implements PreparedSource {
  constructor(
    private readonly engine: Engine,
    private readonly spec: SourceSpec,
    private readonly plot: PlotSpec,
    private readonly vtTable: string,
    private readonly hasId: boolean,
    readonly count: number,
    readonly bounds: DataBounds,
    readonly colorMode: ColorMode,
  ) {}

  async fetchViewport(
    req: ViewportRequest,
  ): Promise<PointColumns & {readonly count: Float64Array}> {
    if (this.count === 0 || req.cols <= 0 || req.rows <= 0) {
      return {
        x: new Float64Array(0),
        y: new Float64Array(0),
        row: new Float64Array(0),
        color: new Float32Array(0),
        count: new Float64Array(0),
      };
    }
    const result = await this.engine.query(viewportQuerySql(this.vtTable, req));
    return decodeViewportColumns(result, this.colorMode);
  }

  async fetchRow(
    id: number,
  ): Promise<ReadonlyArray<{readonly name: string; readonly value: SqlValue}>> {
    const sql =
      this.spec.kind === 'table' && this.hasId
        ? rowFetchSql(this.spec.table, id, 'id')
        : rowFetchByOffsetSql(sourceSubquery(this.spec), id);
    const result = await this.engine.query(sql);
    const it = result.iter({});
    if (!it.valid()) return [];
    return result.columns().map((name) => ({name, value: it.get(name)}));
  }

  async countInRect(rect: DataBounds): Promise<number> {
    const sql = countInRectSql(sourceSubquery(this.spec), this.plot, rect);
    const result = await this.engine.query(sql);
    return result.firstRow({count: NUM}).count;
  }

  sqlForRect(rect: DataBounds): string {
    return sqlForRect(this.spec, this.plot, rect);
  }
}

export class ScatterSource implements AsyncDisposable {
  private nextGen = 0;
  private latestSuccessfulGen = 0;
  private activeResources = new AsyncDisposableStack();
  private disposed = false;

  constructor(
    private readonly engine: Engine,
    private readonly uid: string,
  ) {}

  async describe(spec: SourceSpec): Promise<ColumnInfo[]> {
    if (spec.kind === 'table' && spec.module) {
      await this.engine.query(includeModuleSql(spec.module));
    }
    const result = await this.engine.query(
      `SELECT * FROM ${sourceSubquery(spec)} LIMIT 1000;`,
    );
    const colNames = result.columns();
    if (colNames.length === 0) return [];

    const stats = new Map(
      colNames.map((name) => [
        name,
        {
          hasNonNull: false,
          hasString: false,
          hasNumeric: false,
          hasOther: false,
        },
      ]),
    );

    for (const it = result.iter({}); it.valid(); it.next()) {
      for (const name of colNames) {
        const val = it.get(name);
        if (val === null) continue;
        const st = stats.get(name);
        if (st === undefined) continue;
        st.hasNonNull = true;
        if (typeof val === 'string') {
          st.hasString = true;
        } else if (typeof val === 'number' || typeof val === 'bigint') {
          st.hasNumeric = true;
        } else {
          st.hasOther = true;
        }
      }
    }

    return colNames.map((name) => {
      const st = stats.get(name);
      let kind: ColumnKind = 'other';
      if (st !== undefined) {
        if (st.hasString) {
          kind = 'string';
        } else if (st.hasNonNull && st.hasNumeric && !st.hasOther) {
          kind = 'numeric';
        }
      }
      return {name, kind};
    });
  }

  async prepare(spec: SourceSpec, plot: PlotSpec): Promise<PreparedSource> {
    if (this.disposed) throw new Error('ScatterSource is disposed');
    const gen = ++this.nextGen;
    const vtTable = `__scatter_vt_${this.uid}_${gen}`;
    const genResources = new AsyncDisposableStack();

    try {
      if (spec.kind === 'table' && spec.module) {
        await this.engine.query(includeModuleSql(spec.module));
      }

      const cols = await this.describe(spec);
      const colMap = new Map(cols.map((c) => [c.name, c.kind]));
      for (const [axis, col] of [
        ['X', plot.x],
        ['Y', plot.y],
      ] as const) {
        if (!colMap.has(col)) {
          throw new Error(`${axis} column "${col}" not found in source`);
        }
        if (colMap.get(col) !== 'numeric') {
          throw new Error(
            `${axis} column "${col}" must be numeric, got "${colMap.get(col)}"`,
          );
        }
      }

      const hasId = spec.kind === 'table' && colMap.get('id') === 'numeric';
      let colorMode: ColorMode;
      let isCategorical = false;
      let isDirectColor = false;

      if (plot.color === undefined || plot.color === '') {
        colorMode = {kind: 'none'};
        isDirectColor = true;
      } else {
        if (!colMap.has(plot.color)) {
          throw new Error(`Color column "${plot.color}" not found in source`);
        }
        if (colMap.get(plot.color) === 'numeric') {
          isDirectColor = true;
          colorMode = {kind: 'numeric', column: plot.color, min: 0, max: 0};
        } else {
          isCategorical = true;
          colorMode = {
            kind: 'categorical',
            column: plot.color,
            categories: [],
            counts: [],
            hasOther: false,
            otherCount: 0,
            nullCount: 0,
          };
        }
      }

      let usingArgs: string;
      if (isCategorical) {
        const colorCol = plot.color ?? '';
        let sourceTable: string;
        if (spec.kind === 'table') {
          sourceTable = spec.table;
        } else {
          const tempView = `_scatter_view_${this.uid}_${gen}`;
          genResources.use(
            await createView({
              engine: this.engine,
              name: tempView,
              as: `SELECT * FROM ${sourceSubquery(spec)}`,
            }),
          );
          sourceTable = tempView;
        }
        usingArgs = scatterMipmapTableArgs(
          sourceTable,
          hasId ? 'id' : '',
          plot.x,
          plot.y,
          colorCol,
          TOP_N_CATEGORIES,
        );
      } else {
        const cCol = colorMode.kind === 'numeric' ? plot.color : undefined;
        usingArgs =
          spec.kind === 'table' && hasId && isDirectColor
            ? scatterMipmapTableArgs(spec.table, 'id', plot.x, plot.y, cCol)
            : scatterMipmapSubqueryArgs(spec, plot, cCol);
      }

      genResources.use(
        await createVirtualTable({
          engine: this.engine,
          name: vtTable,
          using: `__intrinsic_scatter_mipmap${usingArgs}`,
        }),
      );

      const [statsRes, catRes] = await Promise.all([
        this.engine.query(vtableStatsQuerySql(vtTable)),
        isCategorical
          ? this.engine.query(vtableCategoriesQuerySql(vtTable))
          : Promise.resolve(undefined),
      ]);

      if (isCategorical && catRes !== undefined) {
        const catRows: Array<{idx: number; value: string; count: number}> = [];
        let hasOther = false;
        let otherCount = 0;
        let nullCount = 0;
        for (
          const it = catRes.iter({out_cat_idx: NUM, out_cat_count: NUM});
          it.valid();
          it.next()
        ) {
          const idx = it.out_cat_idx;
          if (idx === TOP_N_CATEGORIES) {
            hasOther = true;
            otherCount = it.out_cat_count;
          } else if (idx === NULL_CATEGORY) {
            nullCount = it.out_cat_count;
          } else if (idx >= 0 && idx < TOP_N_CATEGORIES) {
            const rawVal = it.get('out_cat_value');
            catRows.push({
              idx,
              value: rawVal === null ? '' : String(rawVal),
              count: it.out_cat_count,
            });
          }
        }
        catRows.sort((a, b) => a.idx - b.idx);
        colorMode = {
          kind: 'categorical',
          column: plot.color ?? '',
          categories: catRows.map((r) => r.value),
          counts: catRows.map((r) => r.count),
          hasOther,
          otherCount,
          nullCount,
        };
      }

      let count = 0;
      let bounds: DataBounds = {xMin: 0, xMax: 0, yMin: 0, yMax: 0};
      let cMin = 0;
      let cMax = 0;

      const statsIt = statsRes.iter({
        out_count: NUM,
        out_x_min: NUM_NULL,
        out_x_max: NUM_NULL,
        out_y_min: NUM_NULL,
        out_y_max: NUM_NULL,
        out_c_min: NUM_NULL,
        out_c_max: NUM_NULL,
      });
      if (statsIt.valid()) {
        count = statsIt.out_count;
        bounds = {
          xMin: fromNumNull(statsIt.out_x_min) ?? 0,
          xMax: fromNumNull(statsIt.out_x_max) ?? 0,
          yMin: fromNumNull(statsIt.out_y_min) ?? 0,
          yMax: fromNumNull(statsIt.out_y_max) ?? 0,
        };
        cMin = fromNumNull(statsIt.out_c_min) ?? 0;
        cMax = fromNumNull(statsIt.out_c_max) ?? 0;
      }

      if (colorMode.kind === 'numeric' && plot.color !== undefined) {
        colorMode = {kind: 'numeric', column: plot.color, min: cMin, max: cMax};
      }

      if (this.disposed || gen < this.latestSuccessfulGen) {
        await genResources.asyncDispose();
        throw new Error(
          this.disposed
            ? 'ScatterSource was disposed during prepare'
            : `Prepare generation ${gen} superseded by generation ${this.latestSuccessfulGen}`,
        );
      }

      const oldResources = this.activeResources;
      this.activeResources = genResources.move();
      this.latestSuccessfulGen = gen;
      await oldResources.asyncDispose();

      return new PreparedSourceImpl(
        this.engine,
        spec,
        plot,
        vtTable,
        hasId,
        count,
        bounds,
        colorMode,
      );
    } catch (err) {
      await genResources.asyncDispose();
      throw err;
    }
  }

  async dispose(): Promise<void> {
    if (this.disposed) return;
    this.disposed = true;
    await this.activeResources.asyncDispose();
  }

  async [Symbol.asyncDispose](): Promise<void> {
    await this.dispose();
  }
}
