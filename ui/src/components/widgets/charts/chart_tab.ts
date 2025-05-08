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
import {addEphemeralTab} from '../../details/add_ephemeral_tab';
import {Tab} from '../../../public/tab';
import {ChartAttrs, renderChart, toTitleCase} from './chart';

export function addChartTab(chart: ChartAttrs): void {
  addEphemeralTab(`${chart.chartType}Tab`, new ChartTab(chart));
}

export class ChartTab implements Tab {
  constructor(private readonly chart: ChartAttrs) {}

  render() {
    return m(
      DetailsShell,
      {
        title:
          this.chart.title !== undefined ? this.chart.title : this.getTitle(),
        description: this.chart.description,
      },
      renderChart(this.chart),
    );
  }

  getTitle(): string {
    return `${toTitleCase(this.chart.columns[0])} ${toTitleCase(this.chart.chartType)}`;
  }
}
