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
import {AsyncMemo, type AtomicTaskQueue} from '../base/async_memo';
import type {AreaSelection} from '../public/selection';
import type {Trace} from '../public/trace';
import {Button} from '../widgets/button';
import {DetailsShell} from '../widgets/details_shell';
import {EmptyState} from '../widgets/empty_state';
import {ExportButton} from '../widgets/export_button';
import {Popup, PopupPosition} from '../widgets/popup';
import {Spinner} from '../widgets/spinner';
import type {
  Aggregation,
  AggregationData,
  Aggregator,
  DataGridState,
} from './aggregation_adapter';
import {AggregationPanel} from './aggregation_panel';
import {AddDebugTrackMenu} from './tracks/add_debug_track_menu';
import type {DataGridApi} from './widgets/datagrid/datagrid';
import type {Column, Filter, Pivot} from './widgets/datagrid/model';
import {SQLDataSource} from './widgets/datagrid/sql_data_source';

export interface DataGridModel {
  readonly columns?: readonly Column[];
  readonly pivot?: Pivot;
  readonly filters: readonly Filter[];
}

interface PreparedAggregation extends AsyncDisposable {
  readonly data: AggregationData;
  readonly dataSource: SQLDataSource;
}

export function createAggregationDataSource(
  trace: Trace,
  aggregator: Aggregator,
  data: AggregationData,
  queue: AtomicTaskQueue,
): SQLDataSource {
  const gridConfig = aggregator.getGridConfig(data);
  const sqlConfig = gridConfig.sqlConfig?.(data) ?? {
    tableOrSubquery: data.tableName,
  };
  return new SQLDataSource({
    queue,
    engine: trace.engine,
    ...sqlConfig,
  });
}

interface AggregationDrilldownPanelAttrs {
  readonly trace: Trace;
  readonly aggregator: Aggregator;
  readonly aggregation: Aggregation;
  readonly selection: AreaSelection;
  readonly queue: AtomicTaskQueue;
  readonly initialDataModel: DataGridModel;
}

export class AggregationDrilldownPanel implements m.ClassComponent<AggregationDrilldownPanelAttrs> {
  private readonly preparedAggregationSlot: AsyncMemo<PreparedAggregation>;
  private readonly initialDataModel: DataGridModel;
  private dataModel: DataGridModel;
  private dataGridApi?: DataGridApi;

  constructor({attrs}: m.Vnode<AggregationDrilldownPanelAttrs>) {
    this.preparedAggregationSlot = new AsyncMemo(attrs.queue);
    this.initialDataModel = attrs.initialDataModel;
    this.dataModel = attrs.initialDataModel;
  }

  view({attrs}: m.Vnode<AggregationDrilldownPanelAttrs>): m.Children {
    const {trace, aggregator, aggregation, selection, queue} = attrs;
    const preparedAggregation = this.preparedAggregationSlot.use({
      key: {
        start: selection.start,
        end: selection.end,
        trackUris: selection.trackUris,
      },
      compute: async () => {
        const data = await aggregation.prepareData(trace.engine);
        const dataSource = createAggregationDataSource(
          trace,
          aggregator,
          data,
          queue,
        );
        return {
          data,
          dataSource,
          [Symbol.asyncDispose]: async () => {
            dataSource.dispose();
            await data[Symbol.asyncDispose]();
          },
        };
      },
    }).data;

    if (!preparedAggregation) {
      return m(
        DetailsShell,
        {
          title: 'Area Selection Drill-down',
          description: aggregator.getTabName(),
        },
        m(
          EmptyState,
          {
            icon: 'mediation',
            title: 'Computing aggregation ...',
            className: 'pf-aggregation-loading',
          },
          m(Spinner, {easing: true}),
        ),
      );
    }

    const {data, dataSource} = preparedAggregation;
    const dataGridState: DataGridState = {
      columns: this.dataModel.columns,
      pivot: this.dataModel.pivot,
      filters: this.dataModel.filters,
      onColumnsChanged: (columns) => {
        this.dataModel = {...this.dataModel, columns};
      },
      onPivotChanged: (pivot) => {
        this.dataModel = {...this.dataModel, pivot};
      },
      onFiltersChanged: (filters) => {
        this.dataModel = {...this.dataModel, filters};
      },
    };

    return m(
      DetailsShell,
      {
        title: 'Area Selection Drill-down',
        description: aggregator.getTabName(),
        buttons: this.renderButtons(trace, dataSource),
      },
      m(AggregationPanel, {
        controls: aggregator.renderTopbarControls?.(),
        dataSource,
        gridConfig: aggregator.getGridConfig(data),
        barChartData: data.barChartData,
        onReady: (api: DataGridApi) => {
          this.dataGridApi = api;
        },
        dataGridState,
        onClearGridState: () => {
          this.dataModel = this.initialDataModel;
        },
      }),
    );
  }

  private renderButtons(trace: Trace, dataSource: SQLDataSource): m.Children {
    const api = this.dataGridApi;
    if (!api) return undefined;

    const model = api.getModel();
    const availableColumns: string[] = [];
    const columnDisplayNames: Record<string, string> = {};
    if (model.mode === 'pivot') {
      for (const {alias, field} of model.groupBy) {
        availableColumns.push(alias);
        columnDisplayNames[alias] = field;
      }
      for (const aggregate of model.aggregates) {
        availableColumns.push(aggregate.alias);
        columnDisplayNames[aggregate.alias] =
          aggregate.function === 'COUNT'
            ? 'COUNT'
            : `${aggregate.function}(${aggregate.field})`;
      }
    } else {
      for (const {alias, field} of model.columns) {
        availableColumns.push(alias);
        columnDisplayNames[alias] = field;
      }
    }
    const query = dataSource.getQuery({...model, pagination: undefined});

    return [
      m(
        Popup,
        {
          trigger: m(Button, {label: 'Add debug track'}),
          position: PopupPosition.Top,
        },
        m(AddDebugTrackMenu, {
          trace,
          query,
          availableColumns,
          columnDisplayNames,
        }),
      ),
      m(ExportButton, {onExportData: api.exportData}),
    ];
  }

  onremove(): void {
    this.preparedAggregationSlot.dispose();
  }
}
