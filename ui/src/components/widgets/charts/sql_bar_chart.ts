// Copyright (C) 2025 The Android Open Source Project
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
import * as vega from 'vega';
import {Trace} from '../../../public/trace';
import {Filters, StandardFilters} from '../sql/table/filters';
import {SqlColumn, sqlColumnId, SqlExpression} from '../sql/table/sql_column';
import {buildSqlQuery} from '../sql/table/query_builder';
import {NUM, SqlValue} from '../../../trace_processor/query_result';
import {Spinner} from '../../../widgets/spinner';
import {VegaLiteSelectionTypes, VegaView} from '../vega_view';
import {stringifyJsonWithBigints} from '../../../base/json_utils';
import {TopLevelSpec} from 'vega-lite';
import {Popup, PopupPosition} from '../../../widgets/popup';
import {raf} from '../../../core/raf_scheduler';
import {Button, ButtonBar} from '../../../widgets/button';
import {AsyncLimiter} from '../../../base/async_limiter';
import {assertDefined} from '../../../base/logging';

interface Data {
  // Raw data from the query.
  raw: {
    value: SqlValue;
    // Vega-lite, unfortunately, does not preserve the type of the data in callbacks, converting numbers to strings and
    // handling NULLs strangely. To avoid problems, we generate unique string ids for each row and use it to identify
    // the selected rows.
    rowId: string;
    count: number;
  }[];
  // A map from unique row ids to the actual values and counts.
  rowIdToValue: Map<string, {value: SqlValue; count: number}>;
}

export class SqlBarChartState {
  private data?: Data;
  private limiter = new AsyncLimiter();

  constructor(
    public readonly args: {
      readonly trace: Trace;
      readonly sqlSource: string;
      readonly filters: Filters;
      readonly column: SqlColumn;
    },
  ) {
    this.reload();
    args.filters.addObserver(() => this.reload());
  }

  private reload() {
    this.limiter.schedule(async () => {
      this.data = undefined;

      const query = buildSqlQuery({
        table: this.args.sqlSource,
        filters: this.args.filters.get(),
        columns: {
          value: this.args.column,
          count: new SqlExpression(() => 'count()', []),
        },
        groupBy: [this.args.column],
      });

      const result = await this.args.trace.engine.query(query);

      const rawData = [];
      for (let it = result.iter({count: NUM}); it.valid(); it.next()) {
        rawData.push({
          value: it.get('value'),
          count: it.count,
          // Add a unique row id to identify the row in the chart.
          rowId: `${rawData.length}`,
        });
      }
      // Sort by count in descending order. We want to sort the data ourselves
      // instead of relying on vega-lite to be able to show only top N rows (coming in the near future).
      rawData.sort((a, b) => b.count - a.count);

      // Map rowIds back to value and count.
      const rowIdToValue = new Map<string, {value: SqlValue; count: number}>();
      for (const d of rawData) {
        rowIdToValue.set(d.rowId, {value: d.value, count: d.count});
      }

      this.data = {
        raw: rawData,
        rowIdToValue,
      };
    });
  }

  getData(): Data | undefined {
    return this.data;
  }
}

export interface SqlBarChartAttrs {
  state: SqlBarChartState;
}

export class SqlBarChart implements m.ClassComponent<SqlBarChartAttrs> {
  lastClickCoordinates?: {x: number; y: number};
  selection: string[] = [];

  view({attrs}: m.Vnode<SqlBarChartAttrs>) {
    const data = attrs.state.getData();
    if (data === undefined) return m(Spinner);
    return m(
      'figure.pf-chart',
      m(VegaView, {
        spec: stringifyJsonWithBigints(this.getVegaSpec(attrs, data)),
        data: {},
        // Listen to 'click' event to determine the position of the popup.
        eventHandlers: [
          {
            name: 'click',
            handler: ({view, event, item}) => {
              const e = event as PointerEvent;
              const i = item as vega.SceneItem | null | undefined;
              const [_, originY] = view.origin();
              this.lastClickCoordinates = {
                x: e.offsetX,
                // Show the popup to the bottom of the selected row.
                y: originY + (i?.bounds?.y2 ?? 0),
              };
              raf.scheduleFullRedraw();
            },
          },
        ],
        // Listen to 'point' signal to monitor changes to the selection.
        signalHandlers: [
          {
            name: VegaLiteSelectionTypes.POINT,
            handler: ({value}) => {
              (this.selection =
                value.rowId === undefined ? [] : [...value.rowId]),
                raf.scheduleFullRedraw();
            },
          },
        ],
        // We rely on listening to signals from vega view to synchronise the selection
        // state between JS and vega, so we need to clear it when the vega view is destroyed.
        onViewDestroyed: () => {
          this.selection = [];
        },
      }),
      m(
        Popup,
        {
          trigger: m('', {
            style: {
              left: this.lastClickCoordinates?.x + 'px',
              top: this.lastClickCoordinates?.y + 'px',
              width: '0px',
              height: '0px',
              position: 'absolute',
            },
          }),
          position: PopupPosition.Bottom,
          isOpen: this.selection.length > 0,
          offset: 5,
        },
        this.renderPopup(attrs, data),
      ),
    );
  }

  // Show a popup with information about each value and allow adding filters.
  renderPopup(attrs: SqlBarChartAttrs, data: Data): m.Children {
    const selectedCount = this.selection
      .map((rowId) => data.rowIdToValue.get(rowId)?.count ?? 0)
      .reduce((a, b) => a + b, 0);
    const total = data.raw.map((d) => d.count).reduce((a, b) => a + b, 0);
    return m(
      '.pf-chart-popup',
      this.selection.length === 1 &&
        m(
          '.pf-chart-popup__tooltip-bold-text',
          `${data.rowIdToValue.get(this.selection[0])?.value}`,
        ),
      this.selection.length > 1 &&
        m(
          '.pf-chart-popup__tooltip-bold-text',
          `${this.selection.length} items: ${this.selection.map((rowId) => data.rowIdToValue.get(rowId)?.value).join(', ')}`,
        ),
      m(
        '.pf-chart-popup__tooltip-text-line',
        `Value: ${selectedCount} (${((selectedCount / total) * 100).toFixed(2)}%)`,
      ),
      m(
        ButtonBar,
        m(Button, {
          label: `Add filter: ${this.selection.length === 1 ? 'equals' : 'is one of'}`,
          onclick: () => {
            attrs.state.args.filters.addFilter(
              StandardFilters.valueIsOneOf(
                attrs.state.args.column,
                this.selection.map((rowId) =>
                  assertDefined(data.rowIdToValue.get(rowId)?.value),
                ),
              ),
            );
          },
        }),
        m(Button, {
          label: `Add filter: not equals`,
          onclick: () => {
            attrs.state.args.filters.addFilters(
              this.selection.map((rowId) =>
                StandardFilters.valueNotEquals(
                  attrs.state.args.column,
                  assertDefined(data.rowIdToValue.get(rowId)?.value),
                ),
              ),
            );
          },
        }),
      ),
    );
  }

  getVegaSpec(attrs: SqlBarChartAttrs, data: Data): TopLevelSpec {
    return {
      $schema: 'https://vega.github.io/schema/vega-lite/v5.json',
      width: 'container',
      mark: 'bar',
      data: {
        values: data.raw,
      },
      params: [
        {
          name: VegaLiteSelectionTypes.POINT,
          select: {
            type: VegaLiteSelectionTypes.POINT,
            fields: ['value', 'rowId'],
          },
        },
      ],
      encoding: {
        y: {
          field: 'value',
          title: sqlColumnId(attrs.state.args.column),
          sort: null,
          axis: {
            labelLimit: 500,
          },
        },
        x: {
          field: 'count',
          type: 'quantitative',
          title: 'Count',
          axis: {
            orient: 'top',
          },
        },
        color: {
          condition: {
            param: VegaLiteSelectionTypes.POINT,
            value: 'red',
            empty: false,
          },
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
      view: {
        strokeWidth: 0,
      },
    };
  }
}
