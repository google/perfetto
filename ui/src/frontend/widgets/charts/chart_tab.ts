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
import {filterTitle} from '../sql/table/column';
import {addEphemeralTab} from '../../../common/add_ephemeral_tab';
import {Tab} from '../../../public/tab';
import {
  ChartConfig,
  ChartOption,
  renderChartComponent,
  toTitleCase,
} from './chart';

export function addChartTab(
  chartOption: ChartOption,
  chartConfig: ChartConfig,
): void {
  addEphemeralTab('histogramTab', new ChartTab(chartOption, chartConfig));
}

export class ChartTab implements Tab {
  constructor(
    private readonly chartOption: ChartOption,
    private readonly chartConfig: ChartConfig,
  ) {}

  render() {
    return m(
      DetailsShell,
      {
        title: this.getTitle(),
        description: this.getDescription(),
      },
      renderChartComponent(this.chartOption, this.chartConfig),
    );
  }

  getTitle(): string {
    return `${toTitleCase(this.chartConfig.columnTitle)} Histogram`;
  }

  private getDescription(): string {
    let desc = `Count distribution for ${this.chartConfig.tableDisplay ?? ''} table`;

    if (this.chartConfig.filters && this.chartConfig.filters.length > 0) {
      desc += ' where ';
      desc += this.chartConfig.filters.map((f) => filterTitle(f)).join(', ');
    }

    return desc;
  }
}
