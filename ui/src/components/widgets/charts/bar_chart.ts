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
import {ChartAttrs} from './chart';
import {VegaLiteSelectionTypes, VegaView} from '../vega_view';
import {Spinner} from '../../../widgets/spinner';
import {stringifyJsonWithBigints} from '../../../base/json_utils';
import {Row} from '../../../trace_processor/query_result';
import {TopLevelSpec} from 'vega-lite';
import {assertExists} from '../../../base/logging';

export class BarChart implements m.ClassComponent<ChartAttrs> {
  getVegaSpec(attrs: ChartAttrs): TopLevelSpec {
    return {
      $schema: 'https://vega.github.io/schema/vega-lite/v5.json',
      width: 'container',
      mark: 'bar',
      data: {
        values: assertExists(attrs.data),
      },
      params: [
        {
          name: VegaLiteSelectionTypes.INTERVAL,
          select: {
            type: VegaLiteSelectionTypes.INTERVAL,
            encodings: ['y'],
          },
        },
        {
          name: VegaLiteSelectionTypes.POINT,
          select: {
            type: VegaLiteSelectionTypes.POINT,
          },
        },
      ],
      encoding: {
        y: {
          field:
            attrs.specProps?.yField !== undefined
              ? attrs.specProps?.yField
              : attrs.columns[0],
          axis: {
            labelLimit: 500,
          },
          sort: {
            op: 'count',
            order: 'descending',
          },
        },
        x: {
          aggregate: 'count',
          title: 'Count',
        },
      },
      config: {
        axisY: {
          titleLineHeight: 15,
          titleBaseline: 'line-bottom',
          titleAngle: 0,
          titleAnchor: 'start',
          titleAlign: 'left',
        },
      },
    };
  }

  view({attrs}: m.Vnode<ChartAttrs>) {
    if (this.isLoading(attrs.data)) {
      return m(Spinner);
    }

    return m(
      'figure',
      {
        className: 'chart',
      },
      m(VegaView, {
        spec: stringifyJsonWithBigints(this.getVegaSpec(attrs)),
        data: {},
        eventHandlers: [
          {
            name: 'click',
            handler: ({item}) => {
              if (item && attrs.onPointSelection !== undefined) {
                attrs.onPointSelection(item);
              }
            },
          },
        ],
        signalHandlers: [
          {
            name: VegaLiteSelectionTypes.INTERVAL,
            handler: ({value}) => {
              if (attrs.onIntervalSelection !== undefined) {
                attrs.onIntervalSelection(value);
              }
            },
          },
        ],
      }),
    );
  }

  isLoading(data?: Row[]): boolean {
    return data === undefined;
  }
}
