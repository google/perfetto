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
import {stringifyJsonWithBigints} from '../../../../base/json_utils';
import {VegaView} from '../../../../widgets/vega_view';
import {Filter} from '../../../widgets/sql/table/column';
import {HistogramState} from './state';
import {Spinner} from '../../../../widgets/spinner';
import {Engine} from '../../../../trace_processor/engine';

export interface HistogramConfig {
  engine: Engine;
  columnTitle: string; // Human readable column name (ex: Duration)
  sqlColumn: string; // SQL column name (ex: dur)
  filters?: Filter[]; // Filters applied to SQL table
  tableDisplay?: string; // Human readable table name (ex: slices)
  query: string; // SQL query for the underlying data
  aggregationType?: 'nominal' | 'quantitative'; // Aggregation type.
}

export class Histogram implements m.ClassComponent<HistogramConfig> {
  private readonly state: HistogramState;

  constructor({attrs}: m.Vnode<HistogramConfig>) {
    this.state = new HistogramState(
      attrs.engine,
      attrs.query,
      attrs.sqlColumn,
      attrs.aggregationType,
    );
  }

  view({attrs}: m.Vnode<HistogramConfig>) {
    const data = this.state.data;
    if (data === undefined) {
      return m(Spinner);
    }

    const {binAxisType, binAxis, countAxis, sort, isBinned, labelLimit} =
      this.state.data!.chartConfig;

    const {sqlColumn, columnTitle} = attrs;

    return m(
      'figure',
      {
        className: 'pf-histogram-view',
      },
      m(VegaView, {
        spec: `{
          "$schema": "https://vega.github.io/schema/vega-lite/v5.json",
          "width": "container",
          "mark": "bar",
          "data": {
            "values": ${stringifyJsonWithBigints(this.state.data!.rows)}
          },
          "encoding": {
            "${binAxis}": {
              "bin": ${isBinned},
              "field": "${sqlColumn}",
              "type": "${binAxisType}",
              "title": "${columnTitle}",
              "sort": ${sort},
              "axis": {
                "labelLimit": ${labelLimit}
              }
            },
            "${countAxis}": {
              "aggregate": "count",
              "title": "Count"
            }
          }
        }`,
        data: {},
      }),
    );
  }

  isLoading(): boolean {
    return this.state.data === undefined;
  }
}
