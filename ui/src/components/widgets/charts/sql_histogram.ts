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
import {SqlColumn, sqlColumnId} from '../sql/table/sql_column';
import {buildSqlQuery} from '../sql/table/query_builder';
import {NUM, NUM_NULL} from '../../../trace_processor/query_result';
import {Spinner} from '../../../widgets/spinner';
import {VegaView} from '../vega_view';
import {stringifyJsonWithBigints} from '../../../base/json_utils';
import {TopLevelSpec} from 'vega-lite';
import {AsyncLimiter} from '../../../base/async_limiter';
import {Callout} from '../../../widgets/callout';
import {Box} from '../../../widgets/box';
import {clamp} from '../../../base/math_utils';
import {uuidv4} from '../../../base/uuid';

interface HistogramData {
  raw: {
    bin_start: number;
    bin_end: number;
    count: number;
  }[];
  min: number;
  max: number;
  binCount: number;
}

interface Data {
  histogram?: HistogramData;
  nullCount: number;
  nonNumericCount: number;
}

export class SqlHistogramState {
  public readonly uuid: string;

  private data?: Data;
  private limiter = new AsyncLimiter();

  constructor(
    public readonly args: {
      readonly trace: Trace;
      readonly sqlSource: string;
      readonly filters: Filters;
      readonly column: SqlColumn;
    },
  ) {
    this.uuid = uuidv4();
    this.reload();
    args.filters.addObserver(() => this.reload());
  }

  private reload() {
    this.limiter.schedule(async () => {
      this.data = undefined;

      // Get the base data using buildSqlQuery
      const dataQuery = buildSqlQuery({
        table: this.args.sqlSource,
        filters: this.args.filters.get(),
        columns: {
          value: this.args.column,
        },
      });

      // Create stats query using CTEs
      const statsQuery = `
        WITH
        source_data AS (
          ${dataQuery}
        ),
        valid_data AS (
          SELECT value
          FROM source_data
          WHERE typeof(value) IN ('integer', 'real')
        ),
        invalid_data AS (
          SELECT value
          FROM source_data
          WHERE typeof(value) NOT IN ('integer', 'real')
        )
        SELECT
          (SELECT MIN(value) FROM valid_data) AS minVal,
          (SELECT MAX(value) FROM valid_data) AS maxVal,
          (SELECT COUNT(*) FROM source_data) AS totalCount,
          (SELECT COUNT(*) FROM valid_data) AS validCount,
          (SELECT COUNT(*) FROM invalid_data WHERE value IS NULL) AS nullCount,
          (SELECT COUNT(*) FROM invalid_data WHERE value IS NOT NULL) AS nonNumericCount
      `;

      const stats = (await this.args.trace.engine.query(statsQuery)).firstRow({
        minVal: NUM_NULL,
        maxVal: NUM_NULL,
        totalCount: NUM,
        validCount: NUM,
        nullCount: NUM,
        nonNumericCount: NUM,
      })!;

      if (
        stats.validCount === 0 ||
        stats.minVal === stats.maxVal ||
        stats.minVal === null ||
        stats.maxVal === null
      ) {
        this.data = {
          nullCount: stats.nullCount,
          nonNumericCount: stats.nonNumericCount,
        };
        return;
      }

      const binCount = (() => {
        // Calculate bin count using Terrell-Scott rule: k = (2 * n)^(1/3)
        if (stats.validCount > 0) {
          return clamp(
            Math.ceil(Math.pow(2 * stats.validCount, 1 / 3)),
            5,
            100,
          );
        }
        return 10;
      })();

      const binWidth = (stats.maxVal - stats.minVal) / binCount;

      // Create histogram query with injected min/max values
      const histogramQuery = `
        WITH
        source_data AS (
          ${dataQuery}
        ),
        bins AS (
          SELECT
            CAST((value - ${stats.minVal}) / ${binWidth} AS INT) AS bin_index,
            ${stats.minVal} + CAST((value - ${stats.minVal}) / ${binWidth} AS INT) * ${binWidth} AS bin_start,
            ${stats.minVal} + (CAST((value - ${stats.minVal}) / ${binWidth} AS INT) + 1) * ${binWidth} AS bin_end
          FROM source_data
          WHERE value IS NOT NULL
            AND typeof(value) IN ('integer', 'real')
        )
        SELECT
          bin_start as binStart,
          bin_end as binEnd,
          COUNT(*) AS count
        FROM bins
        GROUP BY bin_index, binStart, binEnd
        ORDER BY binStart
      `;

      const result = await this.args.trace.engine.query(histogramQuery);

      const rawData = [];
      for (
        let it = result.iter({count: NUM, binStart: NUM, binEnd: NUM});
        it.valid();
        it.next()
      ) {
        rawData.push({
          bin_start: it.binStart,
          bin_end: it.binEnd,
          count: it.count,
        });
      }

      this.data = {
        histogram: {
          raw: rawData,
          min: stats.minVal,
          max: stats.maxVal,
          binCount,
        },
        nullCount: stats.nullCount,
        nonNumericCount: stats.nonNumericCount,
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

    const warning = (value: string) =>
      m(Box, [
        m(
          Callout,
          {
            icon: 'info',
          },
          value,
        ),
      ]);
    const pluralise = (value: number) => (value > 1 ? 's' : '');
    return [
      data.nullCount > 0 &&
        warning(
          `Histogram excludes ${data.nullCount} NULL${pluralise(data.nullCount)}`,
        ),
      data.nonNumericCount > 0 &&
        warning(
          `Histogram excludes ${data.nonNumericCount} non-numeric value${pluralise(data.nonNumericCount)}`,
        ),
      !data.histogram && warning('Nothing to display'),
      data.histogram &&
        m(
          'figure.pf-chart',
          m(VegaView, {
            spec: stringifyJsonWithBigints(
              this.getVegaSpec(attrs, data.histogram),
            ),
            data: {},
          }),
        ),
    ];
  }

  getVegaSpec(
    attrs: SqlHistogramAttrs,
    histogram: HistogramData,
  ): TopLevelSpec {
    return {
      $schema: 'https://vega.github.io/schema/vega-lite/v5.json',
      width: 'container',
      mark: 'bar',
      data: {
        values: histogram.raw,
      },
      encoding: {
        x: {
          field: 'bin_start',
          type: 'quantitative',
          title: sqlColumnId(attrs.state.args.column),
          bin: {
            binned: true,
            step:
              histogram.raw.length > 0
                ? histogram.raw[0].bin_end - histogram.raw[0].bin_start
                : 1,
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
      },
      config: {
        view: {
          strokeWidth: 0,
        },
      },
    };
  }
}
