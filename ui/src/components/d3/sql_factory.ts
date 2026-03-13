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

import {
  ChartSpec,
  ChartType,
  Filter,
  FilterOp,
  AggregationFunction,
} from '../../widgets/charts/d3/data/types';

/**
 * Generates optimized SQLite queries for chart-specific aggregations.
 * Pushes computation down to the database layer for better performance.
 */
export class SqlFactory {
  private static readonly QUERY_LIMIT = 1000;

  constructor(private baseQuery: string) {}

  /**
   * Generate chart-specific SQL with filters applied.
   */
  generateSQL(spec: ChartSpec, filters: Filter[]): string {
    const whereClause = this.buildWhereClause(filters);

    switch (spec.type) {
      case ChartType.Bar:
        return this.generateBarSQL(spec, whereClause);
      case ChartType.Donut:
        return this.generateDonutSQL(spec, whereClause);
      case ChartType.Heatmap:
        return this.generateHeatmapSQL(spec, whereClause);
      case ChartType.Boxplot:
        return this.generateBoxplotSQL(spec, whereClause);
      case ChartType.Histogram:
        return this.generateHistogramSQL(spec, whereClause);
      case ChartType.Cdf:
        return this.generateCdfSQL(spec, whereClause);
      case ChartType.Violin:
        return this.generateViolinSQL(spec, whereClause);
      case ChartType.Line:
        return this.generateLineSQL(spec, whereClause);
      default:
        // No aggregation for scatter
        return this.generateScatterSQL(whereClause);
    }
  }

  private generateBarSQL(
    spec: Extract<ChartSpec, {type: ChartType.Bar}>,
    whereClause: string,
  ): string {
    const groupBy = [this.escapeId(spec.x)];
    if (spec.groupBy) {
      groupBy.push(this.escapeId(spec.groupBy));
    }

    const aggFn = this.getAggFunction(spec.aggregation, spec.y);
    const sortCol = spec.sort?.by === 'x' ? this.escapeId(spec.x) : aggFn;
    const sortDir = spec.sort?.direction === 'asc' ? 'ASC' : 'DESC';

    return `
      WITH base AS (${this.baseQuery})
      SELECT
        ${groupBy.join(', ')},
        ${aggFn} AS ${this.escapeId(spec.y)}
      FROM base
      ${whereClause}
      GROUP BY ${groupBy.join(', ')}
      ORDER BY ${sortCol} ${sortDir}
      LIMIT ${SqlFactory.QUERY_LIMIT}
    `.trim();
  }

  private generateDonutSQL(
    spec: Extract<ChartSpec, {type: ChartType.Donut}>,
    whereClause: string,
  ): string {
    const aggFn = this.getAggFunction(spec.aggregation, spec.value);

    return `
      WITH base AS (${this.baseQuery})
      SELECT
        ${this.escapeId(spec.category)} AS ${this.escapeId(spec.category)},
        ${aggFn} AS ${this.escapeId(spec.value)}
      FROM base
      ${whereClause}
      GROUP BY ${this.escapeId(spec.category)}
      ORDER BY ${aggFn} DESC
      LIMIT ${SqlFactory.QUERY_LIMIT}
    `.trim();
  }

  private generateHeatmapSQL(
    spec: Extract<ChartSpec, {type: ChartType.Heatmap}>,
    whereClause: string,
  ): string {
    const aggFn = this.getAggFunction(spec.aggregation, spec.value);

    return `
      WITH base AS (${this.baseQuery})
      SELECT
        ${this.escapeId(spec.x)} AS ${this.escapeId(spec.x)},
        ${this.escapeId(spec.y)} AS ${this.escapeId(spec.y)},
        ${aggFn} AS ${this.escapeId(spec.value)}
      FROM base
      ${whereClause}
      GROUP BY ${this.escapeId(spec.x)}, ${this.escapeId(spec.y)}
      ORDER BY ${aggFn} DESC
      LIMIT ${SqlFactory.QUERY_LIMIT}
    `.trim();
  }

  private generateBoxplotSQL(
    spec: Extract<ChartSpec, {type: ChartType.Boxplot}>,
    whereClause: string,
  ): string {
    const groupCol = this.escapeId(spec.x);
    const valueCol = this.escapeId(spec.y);

    return `
      WITH cdf_ranked AS (
        SELECT
          ${groupCol} AS grp,
          ${valueCol} AS val,
          CUME_DIST() OVER (
            PARTITION BY ${groupCol}
            ORDER BY ${valueCol}
          ) AS prob
        FROM (${this.baseQuery}) AS t
        ${whereClause}
        ${whereClause ? 'AND' : 'WHERE'} ${valueCol} IS NOT NULL AND ${groupCol} IS NOT NULL
      )
      SELECT
        grp AS "group",
        MIN(val) AS min,
        MIN(CASE WHEN prob >= 0.25 THEN val END) AS q1,
        MIN(CASE WHEN prob >= 0.50 THEN val END) AS median,
        MIN(CASE WHEN prob >= 0.75 THEN val END) AS q3,
        MAX(val) AS max
      FROM cdf_ranked
      GROUP BY grp
    `.trim();
  }

  private generateHistogramSQL(
    spec: Extract<ChartSpec, {type: ChartType.Histogram}>,
    whereClause: string,
  ): string {
    const bins = spec.bins ?? 20;
    const valueCol = this.escapeId(spec.x);
    const maxBinIndex = bins - 1;

    return `
      WITH RECURSIVE
      bin_slots(n) AS (
        SELECT 0
        UNION ALL
        SELECT n + 1 FROM bin_slots WHERE n < ${maxBinIndex}
      ),
      bounds AS (
        SELECT
          MIN(${valueCol}) as mn,
          MAX(${valueCol}) as mx
        FROM (${this.baseQuery}) AS t
        ${whereClause}
        ${whereClause ? 'AND' : 'WHERE'} ${valueCol} IS NOT NULL
      ),
      binned_data AS (
        SELECT
          MIN(${maxBinIndex}, CAST((${maxBinIndex} * 1.0 * (${valueCol} - mn)) / NULLIF(mx - mn, 0) AS INT)) as bin_index
        FROM (${this.baseQuery}) AS t, bounds
        ${whereClause}
        ${whereClause ? 'AND' : 'WHERE'} ${valueCol} IS NOT NULL
      ),
      counts AS (
        SELECT
          bin_index,
          COUNT(*) as cnt
        FROM binned_data
        GROUP BY bin_index
      )
      SELECT
        s.n as bin_index,
        mn + (s.n * (mx - mn) * 1.0 / ${bins}) as x0,
        mn + ((s.n + 1) * (mx - mn) * 1.0 / ${bins}) as x1,
        COALESCE(c.cnt, 0) as count
      FROM bin_slots s
      CROSS JOIN bounds
      LEFT JOIN counts c ON s.n = c.bin_index
      ORDER BY s.n
    `.trim();
  }

  private generateCdfSQL(
    spec: Extract<ChartSpec, {type: ChartType.Cdf}>,
    whereClause: string,
  ): string {
    const valueCol = this.escapeId(spec.x);
    const groupByCol = spec.colorBy ? this.escapeId(spec.colorBy) : null;

    if (groupByCol) {
      // CDF per group (colorBy) - strict percentiles
      return `
        WITH RECURSIVE targets(p) AS (
          SELECT 0.01
          UNION ALL
          SELECT ROUND(p + 0.01, 2) FROM targets WHERE p < 1.00
        ),
        cdf_base AS (
          SELECT
            ${valueCol} as val,
            ${groupByCol} as grp,
            CUME_DIST() OVER (PARTITION BY ${groupByCol} ORDER BY ${valueCol}) as prob
          FROM (${this.baseQuery}) AS t
          ${whereClause}
          ${whereClause ? 'AND' : 'WHERE'} ${valueCol} IS NOT NULL AND ${groupByCol} IS NOT NULL
        )
        SELECT
          t.p as probability,
          MIN(c.val) as value,
          c.grp as "group"
        FROM targets t
        CROSS JOIN (SELECT DISTINCT grp FROM cdf_base) groups
        JOIN cdf_base c ON c.grp = groups.grp AND c.prob >= t.p
        GROUP BY t.p, c.grp
        ORDER BY c.grp, t.p
      `.trim();
    } else {
      // Simple CDF - strict percentiles
      return `
        WITH RECURSIVE targets(p) AS (
          SELECT 0.01
          UNION ALL
          SELECT ROUND(p + 0.01, 2) FROM targets WHERE p < 1.00
        ),
        cdf_base AS (
          SELECT
            ${valueCol} as val,
            CUME_DIST() OVER (ORDER BY ${valueCol}) as prob
          FROM (${this.baseQuery}) AS t
          ${whereClause}
          ${whereClause ? 'AND' : 'WHERE'} ${valueCol} IS NOT NULL
        )
        SELECT
          t.p as probability,
          MIN(c.val) as value
        FROM targets t
        JOIN cdf_base c ON c.prob >= t.p
        GROUP BY t.p
        ORDER BY t.p
      `.trim();
    }
  }

  private generateViolinSQL(
    spec: Extract<ChartSpec, {type: ChartType.Violin}>,
    whereClause: string,
  ): string {
    const groupCol = this.escapeId(spec.x);
    const valueCol = this.escapeId(spec.y);
    const densityPoints = 50;

    return `
      WITH bounds AS (
        SELECT
          ${groupCol} as grp,
          MIN(${valueCol}) as mn,
          MAX(${valueCol}) as mx,
          COUNT(*) as total_cnt
        FROM (${this.baseQuery}) AS t
        ${whereClause}
        ${whereClause ? 'AND' : 'WHERE'} ${valueCol} IS NOT NULL AND ${groupCol} IS NOT NULL
        GROUP BY ${groupCol}
      ),
      cdf_data AS (
        SELECT
          ${groupCol} as grp,
          ${valueCol} as val,
          CUME_DIST() OVER (PARTITION BY ${groupCol} ORDER BY ${valueCol}) as prob
        FROM (${this.baseQuery}) AS t
        ${whereClause}
        ${whereClause ? 'AND' : 'WHERE'} ${valueCol} IS NOT NULL AND ${groupCol} IS NOT NULL
      ),
      density AS (
        SELECT
          ${groupCol} as grp,
          CAST((${valueCol} - b.mn) * ${densityPoints - 1}.0 / NULLIF(b.mx - b.mn, 0) AS INT) as bin,
          b.mn + (CAST((${valueCol} - b.mn) * ${densityPoints - 1}.0 / NULLIF(b.mx - b.mn, 0) AS INT) * (b.mx - b.mn) / ${densityPoints - 1}.0) as bin_value,
          COUNT(*) as cnt
        FROM (${this.baseQuery}) AS t
        JOIN bounds b ON ${groupCol} = b.grp
        ${whereClause}
        ${whereClause ? 'AND' : 'WHERE'} ${valueCol} IS NOT NULL AND ${groupCol} IS NOT NULL
        GROUP BY ${groupCol}, bin, bin_value
      ),
      density_arrays AS (
        SELECT
          grp,
          '[' || GROUP_CONCAT('[' || bin_value || ',' || cnt || ']', ',') || ']' as density
        FROM (
          SELECT grp, bin, bin_value, cnt
          FROM density
          ORDER BY grp, bin
        )
        GROUP BY grp
      )
      SELECT
        b.grp as "group",
        b.mn as min,
        (SELECT MIN(val) FROM cdf_data WHERE grp = b.grp AND prob >= 0.25) as q1,
        (SELECT MIN(val) FROM cdf_data WHERE grp = b.grp AND prob >= 0.50) as median,
        (SELECT MIN(val) FROM cdf_data WHERE grp = b.grp AND prob >= 0.75) as q3,
        (SELECT MIN(val) FROM cdf_data WHERE grp = b.grp AND prob >= 0.90) as p90,
        (SELECT MIN(val) FROM cdf_data WHERE grp = b.grp AND prob >= 0.95) as p95,
        (SELECT MIN(val) FROM cdf_data WHERE grp = b.grp AND prob >= 0.99) as p99,
        b.mx as max,
        da.density as density
      FROM bounds b
      LEFT JOIN density_arrays da ON da.grp = b.grp
    `.trim();
  }

  private generateLineSQL(
    spec: Extract<ChartSpec, {type: ChartType.Line}>,
    whereClause: string,
  ): string {
    const groupBy = [this.escapeId(spec.x)];
    if (spec.colorBy) {
      groupBy.push(this.escapeId(spec.colorBy));
    }

    const aggFn = this.getAggFunction(spec.aggregation, spec.y);
    const sortCol = spec.sort?.by === 'x' ? this.escapeId(spec.x) : aggFn;
    const sortDir = spec.sort?.direction === 'asc' ? 'ASC' : 'DESC';

    return `
      WITH base AS (${this.baseQuery})
      SELECT
        ${groupBy.join(', ')},
        ${aggFn} AS ${this.escapeId(spec.y)}
      FROM base
      ${whereClause}
      GROUP BY ${groupBy.join(', ')}
      ORDER BY ${sortCol} ${sortDir}
      LIMIT ${SqlFactory.QUERY_LIMIT}
    `.trim();
  }

  private generateScatterSQL(whereClause: string): string {
    return `
      SELECT * FROM (${this.baseQuery}) AS t
      ${whereClause}
    `.trim();
  }

  private buildWhereClause(filters: Filter[]): string {
    if (filters.length === 0) return '';

    const conditions = filters.map((f) => this.buildFilterCondition(f));
    return `WHERE ${conditions.join(' AND ')}`;
  }

  private buildFilterCondition(filter: Filter): string {
    const col = this.escapeId(filter.col);
    const {op, val} = filter;

    switch (op) {
      case FilterOp.Eq:
        return `${col} = ${this.escapeLiteral(val)}`;
      case FilterOp.NotEq:
        return `${col} != ${this.escapeLiteral(val)}`;
      case FilterOp.Lt:
        return `${col} < ${this.escapeLiteral(val)}`;
      case FilterOp.Lte:
        return `${col} <= ${this.escapeLiteral(val)}`;
      case FilterOp.Gt:
        return `${col} > ${this.escapeLiteral(val)}`;
      case FilterOp.Gte:
        return `${col} >= ${this.escapeLiteral(val)}`;
      case FilterOp.In:
        if (!Array.isArray(val)) {
          throw new Error('IN operator requires array value');
        }
        const inValues = val.map((v) => this.escapeLiteral(v)).join(', ');
        return `${col} IN (${inValues})`;
      case FilterOp.NotIn:
        if (!Array.isArray(val)) {
          throw new Error('NOT IN operator requires array value');
        }
        const notInValues = val.map((v) => this.escapeLiteral(v)).join(', ');
        return `${col} NOT IN (${notInValues})`;
      case FilterOp.Glob:
        return `${col} GLOB ${this.escapeLiteral(val)}`;
      default:
        throw new Error(`Unsupported filter operator: ${op}`);
    }
  }

  private getAggFunction(fn: AggregationFunction, field: string): string {
    const escapedField = this.escapeId(field);

    switch (fn) {
      case AggregationFunction.Sum:
        return `SUM(${escapedField})`;
      case AggregationFunction.Avg:
        return `AVG(${escapedField})`;
      case AggregationFunction.Count:
        return `COUNT(${escapedField})`;
      case AggregationFunction.Min:
        return `MIN(${escapedField})`;
      case AggregationFunction.Max:
        return `MAX(${escapedField})`;
      default:
        throw new Error(`Unsupported aggregation function: ${fn}`);
    }
  }

  private escapeId(identifier: string): string {
    // SQLite uses double quotes for identifiers
    return `"${identifier.replace(/"/g, '""')}"`;
  }

  private escapeLiteral(
    value: string | number | boolean | string[] | number[] | null,
  ): string {
    if (value === null) {
      return 'NULL';
    }

    if (typeof value === 'boolean') {
      return value ? '1' : '0';
    }

    if (typeof value === 'number') {
      return String(value);
    }

    if (typeof value === 'string') {
      // SQLite uses single quotes for strings, escape single quotes by doubling
      return `'${value.replace(/'/g, "''")}'`;
    }

    throw new Error(`Cannot escape literal value: ${value}`);
  }
}
