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

import m from 'mithril';
import {stringifyJsonWithBigints} from '../../../base/json_utils';
import {uuidv4} from '../../../base/uuid';
import {addBottomTab} from '../../../common/add_ephemeral_tab';
import {DetailsShell} from '../../../widgets/details_shell';
import {Spinner} from '../../../widgets/spinner';
import {VegaView} from '../../../widgets/vega_view';
import {BottomTab, NewBottomTabArgs} from '../../../public/lib/bottom_tab';
import {Filter, filterTitle} from '../../widgets/sql/table/column';
import {HistogramState} from './state';
import {Trace} from '../../../public/trace';

interface HistogramTabConfig {
  columnTitle: string; // Human readable column name (ex: Duration)
  sqlColumn: string; // SQL column name (ex: dur)
  filters?: Filter[]; // Filters applied to SQL table
  tableDisplay?: string; // Human readable table name (ex: slices)
  query: string; // SQL query for the underlying data
  aggregationType?: 'nominal' | 'quantitative'; // Aggregation type.
}

export function addHistogramTab(
  config: HistogramTabConfig,
  trace: Trace,
): void {
  const histogramTab = new HistogramTab({
    config,
    trace,
    uuid: uuidv4(),
  });

  addBottomTab(histogramTab, 'histogramTab');
}

export class HistogramTab extends BottomTab<HistogramTabConfig> {
  static readonly kind = 'dev.perfetto.HistogramTab';

  private state: HistogramState;

  constructor(args: NewBottomTabArgs<HistogramTabConfig>) {
    super(args);

    this.state = new HistogramState(
      this.engine,
      this.config.query,
      this.config.sqlColumn,
      this.config.aggregationType,
    );
  }

  static create(args: NewBottomTabArgs<HistogramTabConfig>): HistogramTab {
    return new HistogramTab(args);
  }

  viewTab() {
    const data = this.state.data;
    if (data === undefined) {
      return m(Spinner);
    }
    return m(
      DetailsShell,
      {
        title: this.getTitle(),
        description: this.getDescription(),
      },
      m(
        '.histogram',
        m(VegaView, {
          spec: `
            {
              "$schema": "https://vega.github.io/schema/vega-lite/v5.json",
              "mark": "bar",
              "data": {
                "values": ${stringifyJsonWithBigints(data.rows)}
              },
              "encoding": {
                "${data.chartConfig.binAxis}": {
                  "bin": ${data.chartConfig.isBinned},
                  "field": "${this.config.sqlColumn}",
                  "type": "${data.chartConfig.binAxisType}",
                  "title": "${this.config.columnTitle}",
                  "sort": ${data.chartConfig.sort},
                  "axis": {
                    "labelLimit": ${data.chartConfig.labelLimit}
                  }
                },
                "${data.chartConfig.countAxis}": {
                  "aggregate": "count",
                  "title": "Count"
                }
              }
            }
          `,
          data: {},
        }),
      ),
    );
  }

  getTitle(): string {
    return `${this.toTitleCase(this.config.columnTitle)} ${
      this.state.data?.chartConfig.binAxisType === 'quantitative'
        ? 'Histogram'
        : 'Counts'
    }`;
  }

  getDescription(): string {
    let desc = `Count distribution for ${this.config.tableDisplay ?? ''} table`;

    if (this.config.filters) {
      desc += ' where ';
      desc += this.config.filters.map((f) => filterTitle(f)).join(', ');
    }

    return desc;
  }

  toTitleCase(s: string): string {
    const words = s.split(/\s/);

    for (let i = 0; i < words.length; ++i) {
      words[i] = words[i][0].toUpperCase() + words[i].substring(1);
    }

    return words.join(' ');
  }

  isLoading(): boolean {
    return this.state.data === undefined;
  }
}
