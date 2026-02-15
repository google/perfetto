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

import m from 'mithril';
import {ChartSpec} from '../../widgets/charts/d3';
import {
  ChartType,
  AggregationFunction,
  LayoutMode,
  SortBy,
  SortDirection,
} from '../../widgets/charts/d3/data/types';
import {Button} from '../../widgets/button';
import {Select} from '../../widgets/select';
import {FormLabel} from '../../widgets/form';

interface ChartCreatorSidebarAttrs {
  availableColumns: string[];
  onClose: () => void;
  onCreate: (type: 'chart' | 'table', spec?: ChartSpec) => void;
}

interface ChartCreatorState {
  selectedType: ChartType | '';
  xColumn: string;
  yColumn: string;
  valueColumn: string;
  categoryColumn: string;
  colorByColumn: string;
  groupByColumn: string;
  aggregation: AggregationFunction;
  bins: number;
  mode: LayoutMode;
  showCorrelation: boolean;
  sortBy: SortBy;
  sortDirection: SortDirection;
}

export class ChartCreatorSidebar
  implements m.ClassComponent<ChartCreatorSidebarAttrs>
{
  private state: ChartCreatorState = {
    selectedType: '',
    xColumn: '',
    yColumn: '',
    valueColumn: '',
    categoryColumn: '',
    colorByColumn: '',
    groupByColumn: '',
    aggregation: AggregationFunction.Sum,
    bins: 20,
    mode: LayoutMode.Grouped,
    showCorrelation: false,
    sortBy: SortBy.X,
    sortDirection: SortDirection.Asc,
  };

  private canCreate(): boolean {
    const {selectedType, xColumn, yColumn, valueColumn, categoryColumn} =
      this.state;

    if (selectedType === '') return false;

    switch (selectedType) {
      case ChartType.Table:
        return true;
      case ChartType.Bar:
        return !!(xColumn && yColumn);
      case ChartType.Histogram:
      case ChartType.Cdf:
        return !!xColumn;
      case ChartType.Scatter:
      case ChartType.Boxplot:
      case ChartType.Violin:
      case ChartType.Line:
        return !!(xColumn && yColumn);
      case ChartType.Heatmap:
        return !!(xColumn && yColumn && valueColumn);
      case ChartType.Donut:
        return !!(categoryColumn && valueColumn);
      default:
        return false;
    }
  }

  private create(attrs: ChartCreatorSidebarAttrs) {
    if (!this.canCreate()) return;

    const {
      selectedType,
      xColumn,
      yColumn,
      valueColumn,
      categoryColumn,
      colorByColumn,
      groupByColumn,
      aggregation,
      bins,
      mode,
      showCorrelation,
      sortBy,
      sortDirection,
    } = this.state;

    if (selectedType === ChartType.Table) {
      attrs.onCreate('table');
      return;
    }

    let spec: ChartSpec;

    switch (selectedType) {
      case ChartType.Bar:
        spec = {
          type: ChartType.Bar,
          x: xColumn,
          y: yColumn,
          aggregation,
          groupBy: groupByColumn || undefined,
          mode: mode,
          sort: {
            by: sortBy,
            direction: sortDirection,
          },
        };
        break;
      case ChartType.Histogram:
        spec = {
          type: ChartType.Histogram,
          x: xColumn,
          bins,
        };
        break;
      case ChartType.Cdf:
        spec = {
          type: ChartType.Cdf,
          x: xColumn,
          colorBy: colorByColumn || undefined,
        };
        break;
      case ChartType.Scatter:
        spec = {
          type: ChartType.Scatter,
          x: xColumn,
          y: yColumn,
          colorBy: colorByColumn || undefined,
          showCorrelation: showCorrelation,
        };
        break;
      case ChartType.Boxplot:
        spec = {
          type: ChartType.Boxplot,
          x: xColumn,
          y: yColumn,
        };
        break;
      case ChartType.Violin:
        spec = {
          type: ChartType.Violin,
          x: xColumn,
          y: yColumn,
        };
        break;
      case ChartType.Line:
        spec = {
          type: ChartType.Line,
          x: xColumn,
          y: yColumn,
          aggregation,
          colorBy: colorByColumn || undefined,
          sort: {
            by: sortBy,
            direction: sortDirection,
          },
        };
        break;
      case ChartType.Heatmap:
        spec = {
          type: ChartType.Heatmap,
          x: xColumn,
          y: yColumn,
          value: valueColumn,
          aggregation,
        };
        break;
      case ChartType.Donut:
        spec = {
          type: ChartType.Donut,
          category: categoryColumn,
          value: valueColumn,
          aggregation,
        };
        break;
      default:
        return;
    }

    attrs.onCreate('chart', spec);
  }

  view({attrs}: m.Vnode<ChartCreatorSidebarAttrs>) {
    return m('.chart-creator-sidebar', [
      m('.sidebar-header', [
        m('h3', 'Add Chart'),
        m(Button, {
          icon: 'close',
          onclick: attrs.onClose,
        }),
      ]),
      m('.sidebar-content', [
        // Chart Type
        m(FormLabel, {class: 'chart-form-field'}, [
          m('span', 'Chart Type'),
          m(
            Select,
            {
              value: this.state.selectedType,
              onchange: (e: Event) => {
                this.state.selectedType = (e.target as HTMLSelectElement)
                  .value as ChartCreatorState['selectedType'];
              },
            },
            [
              m('option', {value: ''}, 'Select chart type...'),
              m('option', {value: ChartType.Table}, 'Table'),
              m('option', {value: ChartType.Bar}, 'Bar Chart'),
              m('option', {value: ChartType.Histogram}, 'Histogram'),
              m('option', {value: ChartType.Cdf}, 'CDF'),
              m('option', {value: ChartType.Scatter}, 'Scatter Plot'),
              m('option', {value: ChartType.Boxplot}, 'Box Plot'),
              m('option', {value: ChartType.Violin}, 'Violin Plot'),
              m('option', {value: ChartType.Line}, 'Line Chart'),
              m('option', {value: ChartType.Heatmap}, 'Heatmap'),
              m('option', {value: ChartType.Donut}, 'Donut Chart'),
            ],
          ),
        ]),

        // X Column
        [
          ChartType.Bar,
          ChartType.Histogram,
          ChartType.Cdf,
          ChartType.Scatter,
          ChartType.Boxplot,
          ChartType.Violin,
          ChartType.Line,
          ChartType.Heatmap,
        ].includes(this.state.selectedType as ChartType) &&
          m(FormLabel, {class: 'chart-form-field'}, [
            m('span', 'X Column'),
            m(
              Select,
              {
                value: this.state.xColumn,
                onchange: (e: Event) => {
                  this.state.xColumn = (e.target as HTMLSelectElement).value;
                },
              },
              [
                m('option', {value: ''}, 'Select column...'),
                ...attrs.availableColumns.map((col) =>
                  m('option', {value: col}, col),
                ),
              ],
            ),
          ]),

        // Y Column
        [
          ChartType.Bar,
          ChartType.Scatter,
          ChartType.Boxplot,
          ChartType.Violin,
          ChartType.Line,
          ChartType.Heatmap,
        ].includes(this.state.selectedType as ChartType) &&
          m(FormLabel, {class: 'chart-form-field'}, [
            m('span', 'Y Column'),
            m(
              Select,
              {
                value: this.state.yColumn,
                onchange: (e: Event) => {
                  this.state.yColumn = (e.target as HTMLSelectElement).value;
                },
              },
              [
                m('option', {value: ''}, 'Select column...'),
                ...attrs.availableColumns.map((col) =>
                  m('option', {value: col}, col),
                ),
              ],
            ),
          ]),

        // Value Column
        [ChartType.Heatmap, ChartType.Donut].includes(
          this.state.selectedType as ChartType,
        ) &&
          m(FormLabel, {class: 'chart-form-field'}, [
            m('span', 'Value Column'),
            m(
              Select,
              {
                value: this.state.valueColumn,
                onchange: (e: Event) => {
                  this.state.valueColumn = (
                    e.target as HTMLSelectElement
                  ).value;
                },
              },
              [
                m('option', {value: ''}, 'Select column...'),
                ...attrs.availableColumns.map((col) =>
                  m('option', {value: col}, col),
                ),
              ],
            ),
          ]),

        // Category Column
        this.state.selectedType === ChartType.Donut &&
          m(FormLabel, {class: 'chart-form-field'}, [
            m('span', 'Category Column'),
            m(
              Select,
              {
                value: this.state.categoryColumn,
                onchange: (e: Event) => {
                  this.state.categoryColumn = (
                    e.target as HTMLSelectElement
                  ).value;
                },
              },
              [
                m('option', {value: ''}, 'Select column...'),
                ...attrs.availableColumns.map((col) =>
                  m('option', {value: col}, col),
                ),
              ],
            ),
          ]),

        // Color By
        [ChartType.Scatter, ChartType.Cdf, ChartType.Line].includes(
          this.state.selectedType as ChartType,
        ) &&
          m(FormLabel, {class: 'chart-form-field'}, [
            m('span', 'Color By (Optional)'),
            m(
              Select,
              {
                value: this.state.colorByColumn,
                onchange: (e: Event) => {
                  this.state.colorByColumn = (
                    e.target as HTMLSelectElement
                  ).value;
                },
              },
              [
                m('option', {value: ''}, 'None'),
                ...attrs.availableColumns.map((col) =>
                  m('option', {value: col}, col),
                ),
              ],
            ),
          ]),

        // Group By
        this.state.selectedType === ChartType.Bar &&
          m(FormLabel, {class: 'chart-form-field'}, [
            m('span', 'Group By (Optional)'),
            m(
              Select,
              {
                value: this.state.groupByColumn,
                onchange: (e: Event) => {
                  this.state.groupByColumn = (
                    e.target as HTMLSelectElement
                  ).value;
                },
              },
              [
                m('option', {value: ''}, 'None'),
                ...attrs.availableColumns.map((col) =>
                  m('option', {value: col}, col),
                ),
              ],
            ),
          ]),

        // Mode
        this.state.selectedType === ChartType.Bar &&
          this.state.groupByColumn !== '' &&
          m(FormLabel, {class: 'chart-form-field'}, [
            m('span', 'Mode'),
            m(
              Select,
              {
                value: this.state.mode,
                onchange: (e: Event) => {
                  this.state.mode = (e.target as HTMLSelectElement)
                    .value as LayoutMode;
                },
              },
              [
                m('option', {value: 'grouped'}, 'Grouped'),
                m('option', {value: 'stacked'}, 'Stacked'),
              ],
            ),
          ]),

        // Sort By
        [ChartType.Bar, ChartType.Line].includes(
          this.state.selectedType as ChartType,
        ) &&
          m(FormLabel, {class: 'chart-form-field'}, [
            m('span', 'Sort By'),
            m(
              Select,
              {
                value: this.state.sortBy,
                onchange: (e: Event) => {
                  this.state.sortBy = (e.target as HTMLSelectElement)
                    .value as SortBy;
                },
              },
              [
                m('option', {value: 'x'}, 'X Column'),
                m('option', {value: 'y'}, 'Y Column'),
              ],
            ),
          ]),

        // Sort Direction
        [ChartType.Bar, ChartType.Line].includes(
          this.state.selectedType as ChartType,
        ) &&
          m(FormLabel, {class: 'chart-form-field'}, [
            m('span', 'Sort Direction'),
            m(
              Select,
              {
                value: this.state.sortDirection,
                onchange: (e: Event) => {
                  this.state.sortDirection = (e.target as HTMLSelectElement)
                    .value as SortDirection;
                },
              },
              [
                m('option', {value: 'asc'}, 'Ascending'),
                m('option', {value: 'desc'}, 'Descending'),
              ],
            ),
          ]),

        // Show Correlation
        this.state.selectedType === ChartType.Scatter &&
          m(FormLabel, {class: 'chart-form-row'}, [
            m('input[type=checkbox].pf-checkbox', {
              checked: this.state.showCorrelation,
              onchange: (e: Event) => {
                this.state.showCorrelation = (
                  e.target as HTMLInputElement
                ).checked;
              },
            }),
            m('span', 'Show Correlation Line'),
          ]),

        // Aggregation
        [
          ChartType.Bar,
          ChartType.Line,
          ChartType.Heatmap,
          ChartType.Donut,
        ].includes(this.state.selectedType as ChartType) &&
          m(FormLabel, {class: 'chart-form-field'}, [
            m('span', 'Aggregation'),
            m(
              Select,
              {
                value: this.state.aggregation,
                onchange: (e: Event) => {
                  this.state.aggregation = (e.target as HTMLSelectElement)
                    .value as AggregationFunction;
                },
              },
              [
                m('option', {value: 'sum'}, 'Sum'),
                m('option', {value: 'avg'}, 'Average'),
                m('option', {value: 'count'}, 'Count'),
                m('option', {value: 'min'}, 'Min'),
                m('option', {value: 'max'}, 'Max'),
              ],
            ),
          ]),

        // Bins
        this.state.selectedType === ChartType.Histogram &&
          m(FormLabel, {class: 'chart-form-field'}, [
            m('span', 'Number of Bins'),
            m('input.pf-text-input[type=number]', {
              value: this.state.bins,
              oninput: (e: Event) => {
                this.state.bins = parseInt(
                  (e.target as HTMLInputElement).value,
                  10,
                );
              },
            }),
          ]),

        // Create Button
        m(Button, {
          label: 'Create Chart',
          icon: 'add_chart',
          disabled: !this.canCreate(),
          onclick: () => this.create(attrs),
        }),
      ]),
    ]);
  }
}
