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
import {MenuItem} from '../../../widgets/menu';
import {Icons} from '../../../base/semantic_icons';
import {Chart, ChartConfig, ChartOption, toTitleCase} from './chart';

interface AddChartMenuItemAttrs {
  readonly chartConfig: ChartConfig;
  readonly chartOptions: Array<ChartOption>;
  readonly addChart: (chart: Chart) => void;
}

export class AddChartMenuItem
  implements m.ClassComponent<AddChartMenuItemAttrs>
{
  private renderAddChartOptions(
    config: ChartConfig,
    chartOptions: Array<ChartOption>,
    addChart: (chart: Chart) => void,
  ): m.Children {
    return chartOptions.map((option) => {
      return m(MenuItem, {
        label: toTitleCase(option),
        onclick: () => addChart({option, config}),
      });
    });
  }

  view({attrs}: m.Vnode<AddChartMenuItemAttrs>) {
    return m(
      MenuItem,
      {label: 'Add chart', icon: Icons.Chart},
      this.renderAddChartOptions(
        attrs.chartConfig,
        attrs.chartOptions,
        attrs.addChart,
      ),
    );
  }
}
