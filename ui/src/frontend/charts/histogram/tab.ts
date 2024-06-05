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

import {DetailsShell} from '../../../widgets/details_shell';
import {uuidv4} from '../../../base/uuid';
import {BottomTab, NewBottomTabArgs} from '../../bottom_tab';
import {VegaView} from '../../../widgets/vega_view';
import {addEphemeralTab} from '../../../common/addEphemeralTab';
import {HistogramState} from './state';
import {stringifyJsonWithBigints} from '../../../base/json_utils';
import {Engine} from '../../../public';
import {Filter} from '../../sql_table/state';
import {isString} from '../../../base/object_utils';

interface HistogramTabConfig {
  columnTitle: string; // Human readable column name (ex: Duration)
  sqlColumn: string; // SQL column name (ex: dur)
  filters?: Filter[]; // Filters applied to SQL table
  tableDisplay?: string; // Human readable table name (ex: slices)
  query: string; // SQL query for the underlying data
}

export function addHistogramTab(
  config: HistogramTabConfig,
  engine: Engine,
): void {
  const histogramTab = new HistogramTab({
    config,
    engine,
    uuid: uuidv4(),
  });

  addEphemeralTab(histogramTab, 'histogramTab');
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
    );
  }

  static create(args: NewBottomTabArgs<HistogramTabConfig>): HistogramTab {
    return new HistogramTab(args);
  }

  viewTab() {
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
                "values": ${
                  this.state.data
                    ? stringifyJsonWithBigints(this.state.data)
                    : []
                }
              },
              "encoding": {
                "${this.state.chartConfig.binAxis}": {
                  "bin": ${this.state.chartConfig.isBinned},
                  "field": "${this.config.sqlColumn}",
                  "type": "${this.state.chartConfig.binAxisType}",
                  "title": "${this.config.columnTitle}",
                  "sort": ${this.state.chartConfig.sort},
                  "axis": {
                    "labelLimit": ${this.state.chartConfig.labelLimit}
                  }
                },
                "${this.state.chartConfig.countAxis}": {
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
      this.state.chartConfig.binAxisType === 'quantitative'
        ? 'Histogram'
        : 'Counts'
    }`;
  }

  getDescription(): string {
    let desc = `Count distribution for ${
      this.config.tableDisplay ? this.config.tableDisplay : ''
    } table`;

    if (this.config.filters) {
      const filterStrings: string[] = [];
      desc += ' where ';

      for (const f of this.config.filters) {
        filterStrings.push(`${isString(f) ? f : `${f.argName} ${f.op}`}`);
      }

      desc += filterStrings.join(', ');
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
    return this.state.isLoading;
  }
}
