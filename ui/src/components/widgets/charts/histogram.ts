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
import {VegaLiteSelectionTypes, VegaView} from '../vega_view';
import {Spinner} from '../../../widgets/spinner';
import {ChartAttrs} from './chart';
import {Row} from '../../../trace_processor/query_result';
import {TopLevelSpec} from 'vega-lite';
import {assertExists} from '../../../base/logging';

type HistogramAttrs = ChartAttrs;

export class Histogram implements m.ClassComponent<HistogramAttrs> {
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
            encodings: ['x'],
          },
        },
        {
          name: VegaLiteSelectionTypes.POINT,
          select: {type: VegaLiteSelectionTypes.POINT},
        },
      ],
      encoding: {
        x: {
          bin: true,
          field: attrs.columns[0],
          title: attrs.columns[0],
          axis: {
            labelLimit: 500,
          },
        },
        y: {
          aggregate: 'count',
          title: 'Count',
        },
      },
    };
  }

  view({attrs}: m.Vnode<HistogramAttrs>) {
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
