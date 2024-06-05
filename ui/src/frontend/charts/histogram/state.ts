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
import {Engine} from '../../../public';
import {Row} from '../../../trace_processor/query_result';

interface ChartConfig {
  binAxisType?: 'nominal' | 'quantitative';
  binAxis: 'x' | 'y';
  countAxis: 'x' | 'y';
  sort: string;
  isBinned: boolean;
  labelLimit?: number;
}

export class HistogramState {
  private readonly sqlColumn: string;
  private readonly engine: Engine;
  private readonly query: string;

  data?: Row[];
  chartConfig: ChartConfig;

  get isLoading() {
    return this.data === undefined;
  }

  constructor(engine: Engine, query: string, column: string) {
    this.engine = engine;
    this.query = query;
    this.sqlColumn = column;

    this.chartConfig = {
      binAxis: 'x',
      binAxisType: 'nominal',
      countAxis: 'y',
      sort: 'false',
      isBinned: true,
      labelLimit: 500,
    };

    this.getData();
  }

  async getData() {
    const res = await this.engine.query(`
      SELECT ${this.sqlColumn}
      FROM (
        ${this.query}
      )
    `);

    const rows: Row[] = [];

    for (const it = res.iter({}); it.valid(); it.next()) {
      const rowVal = it.get(this.sqlColumn);

      if (
        this.chartConfig.binAxisType === 'nominal' &&
        typeof rowVal === 'bigint'
      ) {
        this.chartConfig.binAxisType = 'quantitative';
      }

      rows.push({
        [this.sqlColumn]: rowVal,
      });
    }

    this.data = rows;

    if (this.chartConfig.binAxisType === 'nominal') {
      this.chartConfig.binAxis = 'y';
      this.chartConfig.countAxis = 'x';
      this.chartConfig.sort = `{
          "op": "count",
          "order": "descending"
        }`;
      this.chartConfig.isBinned = false;
    }

    raf.scheduleFullRedraw();
  }
}
