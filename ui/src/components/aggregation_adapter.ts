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
import {AsyncLimiter} from '../base/async_limiter';
import {time, Time} from '../base/time';
import {exists} from '../base/utils';
import {
  AreaSelection,
  areaSelectionsEqual,
  AreaSelectionTab,
} from '../public/selection';
import {Trace} from '../public/trace';
import {Track} from '../public/track';
import {UnionDataset, Dataset, DatasetSchema} from '../trace_processor/dataset';
import {Engine} from '../trace_processor/engine';
import {EmptyState} from '../widgets/empty_state';
import {Spinner} from '../widgets/spinner';
import {AggregationPanel} from './aggregation_panel';
import {Pivot} from './widgets/datagrid/model';
import {SQLDataSource} from './widgets/datagrid/sql_data_source';
import {createSimpleSchema} from './widgets/datagrid/sql_schema';
import {BarChartData, ColumnDef} from './aggregation';
import {
  createPerfettoTable,
  DisposableSqlEntity,
} from '../trace_processor/sql_utils';
import {DataGridApi} from './widgets/datagrid/datagrid';
import {DataGridExportButton} from './widgets/datagrid/export_button';
import {DataSource} from './widgets/datagrid/data_source';

export interface AggregationData {
  readonly tableName: string;
  readonly barChartData?: ReadonlyArray<BarChartData>;
}

export interface Aggregation {
  /**
   * Creates a view for the aggregated data corresponding to the selected area.
   *
   * The dataset provided will be filtered based on the `trackKind` and `schema`
   * if these properties are defined.
   *
   * @param engine - The query engine used to execute queries.
   */
  prepareData(engine: Engine): Promise<AggregationData>;
}

export interface AggregatePivotModel extends Pivot {
  readonly columns: ReadonlyArray<ColumnDef>;
}

export interface Aggregator {
  readonly id: string;

  /**
   * If set, this component will be used instead of the default AggregationPanel
   * for displaying the aggregation. Use this to customize the look and feel of
   * the rendered table.
   */
  readonly Panel?: PanelComponent;

  /**
   * This function is called every time the area selection changes. The purpose
   * of this function is to test whether this aggregator applies to the given
   * area selection. If it does, it returns an aggregation object which gives
   * further instructions on how to prepare the aggregation data.
   *
   * Aggregators are arranged this way because often the computation required to
   * work out whether this aggregation applies is the same as the computation
   * required to actually do the aggregation, so doing it like this means the
   * prepareData() function returned can capture intermediate state avoiding
   * having to do it again or awkwardly cache it somewhere in the aggregators
   * local state.
   */
  probe(area: AreaSelection): Aggregation | undefined;
  getTabName(): string;
  getColumnDefinitions(): ColumnDef[] | AggregatePivotModel;

  /**
   * Optionally override which component is used to render the data in the
   * details panel. This can be used to define customize how the data is
   * rendered.
   */
  readonly PanelComponent?: PanelComponent;
}

export interface AggregationPanelAttrs {
  readonly dataSource: DataSource;
  readonly columns: ReadonlyArray<ColumnDef> | AggregatePivotModel;
  readonly barChartData?: ReadonlyArray<BarChartData>;
  readonly onReady?: (api: DataGridApi) => void;
}

// Define a type for the expected props of the panel components so that a
// generic AggregationPanel can be specificed as an argument to
// createBaseAggregationToTabAdaptor()
export type PanelComponent = m.ComponentTypes<AggregationPanelAttrs>;

export function selectTracksAndGetDataset<T extends DatasetSchema>(
  tracks: ReadonlyArray<Track>,
  spec: T,
  kind?: string,
) {
  const datasets = tracks
    .filter((t) => kind === undefined || t.tags?.kinds?.includes(kind))
    .map((t) => t.renderer.getDataset?.())
    .filter(exists)
    .filter((d) => d.implements(spec));

  if (datasets.length > 0) {
    return UnionDataset.create(datasets);
  } else {
    return undefined;
  }
}

/**
 * For a given slice-like dataset (ts, dur and id cols), creates a new table
 * that contains the slices intersected with a given interval.
 *
 * @param engine The engine to use to run queries.
 * @param dataset The source dataset.
 * @param start The start of the interval to intersect with.
 * @param end The end of the interval to intersect with.
 * @returns A disposable SQL entity representing the new table.
 */
export async function createIITable<
  T extends {ts: bigint; dur: bigint; id: number},
>(
  engine: Engine,
  dataset: Dataset<T>,
  start: time,
  end: time,
): Promise<DisposableSqlEntity> {
  const duration = Time.durationBetween(start, end);

  if (duration <= 0n) {
    // Return an empty dataset if the area selection's length is zero or less.
    // II can't handle 0 or negative durations.
    return createPerfettoTable({
      engine,
      as: `
        SELECT * 
        FROM (${dataset.query()})
        LIMIT 0
      `,
    });
  }

  // Materialize the source into a perfetto table first, dropping all incomplete
  // slices.
  //
  // Note: the `ORDER BY id` is absolutely crucial. Removing this significantly
  // worsens aggregation results compared to no materialization at all.
  await using tempTable = await createPerfettoTable({
    engine,
    as: `
      WITH slices AS (${dataset.query()})
      SELECT * FROM slices
      WHERE dur >= 0
      ORDER BY id
    `,
  });

  // Include all columns from the dataset except for `dur` and `ts`, which
  // are replaced with the `dur` and `ts` from the interval intersection.
  const otherCols = Object.keys(dataset.schema).filter(
    (col) => col !== 'dur' && col !== 'ts',
  );

  await engine.query(`INCLUDE PERFETTO MODULE intervals.intersect`);
  return await createPerfettoTable({
    engine,
    as: `
      SELECT
        ${otherCols.map((c) => `slices.${c}`).join()},
        ii.dur AS dur,
        ii.ts AS ts
      FROM _interval_intersect_single!(
        ${start},
        ${duration},
        ${tempTable.name}
      ) AS ii
      JOIN ${tempTable.name} AS slices USING (id)
    `,
  });
}

/**
 * Creates an adapter that adapts an old style aggregation to a new area
 * selection sub-tab.
 */
export function createAggregationTab(
  trace: Trace,
  aggregator: Aggregator,
  priority: number = 0,
): AreaSelectionTab {
  const limiter = new AsyncLimiter();
  let currentSelection: AreaSelection | undefined;
  let aggregation: Aggregation | undefined;
  let data: AggregationData | undefined;
  let dataSource: SQLDataSource | undefined;
  let dataGridApi: DataGridApi | undefined;

  return {
    id: aggregator.id,
    name: aggregator.getTabName(),
    priority,
    render(selection: AreaSelection) {
      if (
        currentSelection === undefined ||
        !areaSelectionsEqual(selection, currentSelection)
      ) {
        // Every time the selection changes, probe the aggregator to see if it
        // supports this selection.
        currentSelection = selection;
        aggregation = aggregator.probe(selection);

        // Kick off a new load of the data
        limiter.schedule(async () => {
          // Clear previous data to prevent queries against a stale or partially
          // updated table/view while `prepareData` is running.
          dataSource = undefined;
          data = undefined;
          if (aggregation) {
            data = await aggregation?.prepareData(trace.engine);
            dataSource = new SQLDataSource({
              engine: trace.engine,
              sqlSchema: createSimpleSchema(data.tableName),
              rootSchemaName: 'query',
            });
          }
        });
      }

      if (!aggregation) {
        // Hides the tab
        return undefined;
      }

      if (!dataSource) {
        return {
          isLoading: true,
          content: m(
            EmptyState,
            {
              icon: 'mediation',
              title: 'Computing aggregation ...',
              className: 'pf-aggregation-loading',
            },
            m(Spinner, {easing: true}),
          ),
        };
      }

      const PanelComponent = aggregator.Panel ?? AggregationPanel;

      return {
        isLoading: false,
        content: m(PanelComponent, {
          key: aggregator.id,
          dataSource,
          columns: aggregator.getColumnDefinitions(),
          barChartData: data?.barChartData,
          onReady: (api: DataGridApi) => {
            dataGridApi = api;
          },
        }),
        buttons:
          dataGridApi &&
          m(DataGridExportButton, {onExportData: dataGridApi.exportData}),
      };
    },
  };
}
