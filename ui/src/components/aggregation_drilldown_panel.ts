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
import {AtomicTaskQueue} from '../base/async_memo';
import type {Trace} from '../public/trace';
import {DetailsShell} from '../widgets/details_shell';
import {ExportButton} from '../widgets/export_button';
import type {
  AggregationData,
  Aggregator,
  AggregatorGridConfig,
  DataGridState,
} from './aggregation_adapter';
import {AggregationPanel} from './aggregation_panel';
import type {DataGridApi} from './widgets/datagrid/datagrid';
import type {Column, Filter, Pivot} from './widgets/datagrid/model';
import {SQLDataSource} from './widgets/datagrid/sql_data_source';
import type {SharedAsyncDisposable} from '../base/shared_disposable';
import type {DisposableSqlEntity} from '../trace_processor/sql_utils';

export interface DataGridModel {
  readonly columns?: readonly Column[];
  readonly pivot?: Pivot;
  readonly filters: readonly Filter[];
}

export function createAggregationDataSource(
  trace: Trace,
  gridConfig: AggregatorGridConfig,
  data: AggregationData,
  queue: AtomicTaskQueue,
): SQLDataSource {
  const sqlConfig = gridConfig.sqlConfig?.(data) ?? {
    tableOrSubquery: data.sqlTable.get().name,
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
  readonly gridConfig: AggregatorGridConfig;
  readonly initialDataModel: DataGridModel;
  readonly sharedTable: SharedAsyncDisposable<DisposableSqlEntity>;
  readonly aggregationData: AggregationData;
}

export class AggregationDrilldownPanel implements m.ClassComponent<AggregationDrilldownPanelAttrs> {
  private readonly initialDataModel: DataGridModel;
  private dataModel: DataGridModel;
  private dataGridApi?: DataGridApi;
  private table: SharedAsyncDisposable<DisposableSqlEntity>;
  private datasource: SQLDataSource;

  constructor({attrs}: m.Vnode<AggregationDrilldownPanelAttrs>) {
    this.initialDataModel = attrs.initialDataModel;
    this.dataModel = attrs.initialDataModel;
    this.table = attrs.sharedTable.clone();
    this.datasource = createAggregationDataSource(
      attrs.trace,
      attrs.gridConfig,
      attrs.aggregationData,
      new AtomicTaskQueue(),
    );
  }

  view({attrs}: m.Vnode<AggregationDrilldownPanelAttrs>): m.Children {
    const {aggregator, gridConfig} = attrs;
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
        buttons: this.renderButtons(),
      },
      m(AggregationPanel, {
        controls: aggregator.renderTopbarControls?.(),
        dataSource: this.datasource,
        gridConfig,
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

  private renderButtons(): m.Children {
    const api = this.dataGridApi;
    if (!api) return undefined;

    return m(ExportButton, {onExportData: api.exportData});
  }

  onremove(): void {
    this.table[Symbol.asyncDispose]();
  }
}
