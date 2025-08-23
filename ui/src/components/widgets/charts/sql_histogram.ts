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

import m from 'mithril';
import {Trace} from '../../../public/trace';
import {Filters} from '../sql/table/filters';
import {SqlColumn, sqlColumnId, SqlExpression} from '../sql/table/sql_column';
import {buildSqlQuery} from '../sql/table/query_builder';
import {NUM} from '../../../trace_processor/query_result';
import {Spinner} from '../../../widgets/spinner';
import {VegaView} from '../vega_view';
import {stringifyJsonWithBigints} from '../../../base/json_utils';
import {TopLevelSpec} from 'vega-lite';
import {AsyncLimiter} from '../../../base/async_limiter';

interface Data {
  raw: {
    bin_start: number;
    bin_end: number;
    count: number;
  }[];
  min: number;
  max: number;
  binCount: number;
}

export class SqlHistogramState {
  private data?: Data;
  private limiter = new AsyncLimiter();

  constructor(
    public readonly args: {
      readonly trace: Trace;
      readonly sqlSource: string;
      readonly filters: Filters;
      readonly column: SqlColumn;
      readonly binCount?: number;
    },
  ) {
    this.reload();
    args.filters.addObserver(() => this.reload());
  }

  private reload() {
    this.limiter.schedule(async () => {
      this.data = undefined;

      const binCount = this.args.binCount ?? 20;

      // First, get min/max values
      const statsQuery = buildSqlQuery({
        table: this.args.sqlSource,
        filters: this.args.filters.get(),
        columns: {
          min_val: new SqlExpression(
            (cols: string[]) => `MIN(${cols[0]})`,
            [this.args.column],
          ),
          max_val: new SqlExpression(
            (cols: string[]) => `MAX(${cols[0]})`,
            [this.args.column],
          ),
          count: new SqlExpression(
            () => 'COUNT(*)',
            [],
          ),
        },
      });

      const statsResult = await this.args.trace.engine.query(statsQuery);
      const statsIt = statsResult.iter({});
      if (!statsIt.valid()) {
        this.data = {
          raw: [],
          min: 0,
          max: 0,
          binCount: 0,
        };
        return;
      }

      const minVal = Number(statsIt.get('min_val') ?? 0);
      const maxVal = Number(statsIt.get('max_val') ?? 0);
      const totalCount = Number(statsIt.get('count') ?? 0);

      if (totalCount === 0 || minVal === maxVal) {
        this.data = {
          raw: [{
            bin_start: minVal,
            bin_end: maxVal,
            count: totalCount,
          }],
          min: minVal,
          max: maxVal,
          binCount: 1,
        };
        return;
      }

      const binWidth = (maxVal - minVal) / binCount;

      // Get the data using buildSqlQuery
      const dataQuery = buildSqlQuery({
        table: this.args.sqlSource,
        filters: this.args.filters.get(),
        columns: {
          value: this.args.column,
        },
      });

      // Create histogram query with injected min/max values
      const histogramQuery = `
        WITH 
        source_data AS (
          ${dataQuery}
        ),
        bins AS (
          SELECT 
            CAST((value - ${minVal}) / ${binWidth} AS INT) AS bin_index,
            ${minVal} + CAST((value - ${minVal}) / ${binWidth} AS INT) * ${binWidth} AS bin_start,
            ${minVal} + (CAST((value - ${minVal}) / ${binWidth} AS INT) + 1) * ${binWidth} AS bin_end
          FROM source_data
          WHERE value IS NOT NULL
        )
        SELECT 
          bin_start,
          bin_end,
          COUNT(*) AS count
        FROM bins
        GROUP BY bin_index, bin_start, bin_end
        ORDER BY bin_start
      `;

      const result = await this.args.trace.engine.query(histogramQuery);

      const rawData = [];
      for (let it = result.iter({count: NUM}); it.valid(); it.next()) {
        const binStart = Number(it.get('bin_start'));
        const binEnd = Number(it.get('bin_end'));
        rawData.push({
          bin_start: binStart,
          bin_end: binEnd,
          count: it.count,
        });
      }

      this.data = {
        raw: rawData,
        min: minVal,
        max: maxVal,
        binCount,
      };
    });
  }

  getData(): Data | undefined {
    return this.data;
  }
}

export interface SqlHistogramAttrs {
  state: SqlHistogramState;
}

export class SqlHistogram implements m.ClassComponent<SqlHistogramAttrs> {
  view({attrs}: m.Vnode<SqlHistogramAttrs>) {
    const data = attrs.state.getData();
    if (data === undefined) return m(Spinner);
    return m(
      'figure.pf-chart',
      m(VegaView, {
        spec: stringifyJsonWithBigints(this.getVegaSpec(attrs, data)),
        data: {},
      }),
    );
  }

  getVegaSpec(attrs: SqlHistogramAttrs, data: Data): TopLevelSpec {
    return {
      $schema: 'https://vega.github.io/schema/vega-lite/v5.json',
      width: 'container',
      mark: 'bar',
      data: {
        values: data.raw,
      },
      encoding: {
        x: {
          field: 'bin_start',
          type: 'quantitative',
          title: sqlColumnId(attrs.state.args.column),
          bin: {
            binned: true,
            step: data.raw.length > 0 ? data.raw[0].bin_end - data.raw[0].bin_start : 1,
          },
          axis: {
            labelLimit: 500,
          },
        },
        x2: {
          field: 'bin_end',
        },
        y: {
          field: 'count',
          type: 'quantitative',
          title: 'Count',
        },
        tooltip: [
          {field: 'bin_start', type: 'quantitative', format: '.2f', title: 'Start'},
          {field: 'bin_end', type: 'quantitative', format: '.2f', title: 'End'},
          {field: 'count', type: 'quantitative', title: 'Count'},
        ],
      },
      config: {
        view: {
          strokeWidth: 0,
        },
      },
    };
  }
}