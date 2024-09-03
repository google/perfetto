// Copyright (C) 2024 The Android Open Source Project
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
import {raf} from '../../../core/raf_scheduler';
import {Engine} from '../../../trace_processor/engine';
import {Row} from '../../../trace_processor/query_result';

interface ChartConfig {
  binAxisType: 'nominal' | 'quantitative';
  binAxis: 'x' | 'y';
  countAxis: 'x' | 'y';
  sort: string;
  isBinned: boolean;
  labelLimit?: number;
}

interface HistogramData {
  readonly rows: Row[];
  readonly error?: string;
  readonly chartConfig: ChartConfig;
}

function getHistogramConfig(
  aggregationType: 'nominal' | 'quantitative',
): ChartConfig {
  const labelLimit = 500;
  if (aggregationType === 'nominal') {
    return {
      binAxisType: aggregationType,
      binAxis: 'y',
      countAxis: 'x',
      sort: `{
        "op": "count",
        "order": "descending"
      }`,
      isBinned: false,
      labelLimit,
    };
  } else {
    return {
      binAxisType: aggregationType,
      binAxis: 'x',
      countAxis: 'y',
      sort: 'false',
      isBinned: true,
      labelLimit,
    };
  }
}

export class HistogramState {
  data?: HistogramData;

  constructor(
    private readonly engine: Engine,
    private readonly query: string,
    private readonly sqlColumn: string,
    private readonly aggregationType?: 'nominal' | 'quantitative',
  ) {
    this.loadData();
  }

  private async loadData() {
    const res = await this.engine.query(`
      SELECT ${this.sqlColumn}
      FROM (
        ${this.query}
      )
    `);

    const rows: Row[] = [];

    let hasQuantitativeData = false;

    for (const it = res.iter({}); it.valid(); it.next()) {
      const rowVal = it.get(this.sqlColumn);
      if (typeof rowVal === 'bigint') {
        hasQuantitativeData = true;
      }

      rows.push({
        [this.sqlColumn]: rowVal,
      });
    }

    const aggregationType =
      this.aggregationType !== undefined
        ? this.aggregationType
        : hasQuantitativeData
          ? 'quantitative'
          : 'nominal';

    this.data = {
      rows,
      chartConfig: getHistogramConfig(aggregationType),
    };

    raf.scheduleFullRedraw();
  }
}
