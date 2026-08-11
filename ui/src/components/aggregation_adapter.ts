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

import './aggregation_adapter.scss';
import m from 'mithril';
import {type time, Time} from '../base/time';
import {exists} from '../base/utils';
import type {AreaSelection, AreaSelectionTab} from '../public/selection';
import type {Trace} from '../public/trace';
import type {Track} from '../public/track';
import {
  UnionDataset,
  type Dataset,
  type DatasetSchema,
} from '../trace_processor/dataset';
import type {Engine} from '../trace_processor/engine';
import {EmptyState} from '../widgets/empty_state';
import {Spinner} from '../widgets/spinner';
import {
  AggregationDrilldownPanel,
  type DataGridModel,
} from './aggregation_drilldown_panel';
import {AggregationPanel} from './aggregation_panel';
import {addEphemeralTab} from './details/add_ephemeral_tab';
import type {Column, Filter, Pivot} from './widgets/datagrid/model';
import {SQLDataSource} from './widgets/datagrid/sql_data_source';
import type {SQLTableSchema} from './widgets/datagrid/sql_schema';
import type {BarChartData} from './aggregation';
import {
  createPerfettoTable,
  type DisposableSqlEntity,
} from '../trace_processor/sql_utils';
import type {DataGridApi} from './widgets/datagrid/datagrid';
import {ExportButton} from '../widgets/export_button';
import {AsyncMemo, AtomicTaskQueue} from '../base/async_memo';
import type {ColumnSchema} from './widgets/datagrid/datagrid_schema';
import {Memo} from '../base/memo';
import {assertExists} from '../base/assert';
import {Button, ButtonGroup} from '../widgets/button';
import {SharedAsyncDisposable} from '../base/shared_disposable';

export interface AggregationData extends AsyncDisposable {
  readonly sqlTable: SharedAsyncDisposable<DisposableSqlEntity>;
  readonly barChartData?: ReadonlyArray<BarChartData>;
}

export function createAggregationData(
  table: DisposableSqlEntity,
  barChartData?: ReadonlyArray<BarChartData>,
): AggregationData {
  const sharedTable = SharedAsyncDisposable.wrap(table);
  return {
    sqlTable: sharedTable,
    barChartData,
    [Symbol.asyncDispose]: () => sharedTable[Symbol.asyncDispose](),
  };
}

export interface AggregatorGridPreset {
  readonly displayName: string;
  readonly config: AggregatorGridConfig;
}

export interface Aggregation {
  /**
   * Defines how the datagrid that displays the aggregated data looks, including
   * what to pivot on and the column definitions.
   */
  getGridConfig(): AggregatorGridConfig | readonly AggregatorGridPreset[];

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

/**
 * Initial configuration for the DataGrid in an aggregation panel.
 */
export interface AggregatorGridConfig {
  readonly schema: ColumnSchema;
  readonly initialColumns?: readonly Column[];
  readonly initialPivot?: Pivot;
  readonly initialFilters?: readonly Filter[];
  /**
   * Optional override that produces the SQL schema used to resolve columns
   * for the aggregation table. If undefined, a simple table-or-subquery
   * schema is created from the table name.
   */
  readonly sqlConfig?: (data: AggregationData) => SQLTableSchema;
}

/**
 * State that can be controlled externally for a DataGrid.
 * All properties are optional - only those provided will be used in controlled
 * mode, others will use uncontrolled (internal) state.
 */
export interface DataGridState {
  readonly columns?: readonly Column[];
  readonly filters?: readonly Filter[];
  readonly pivot?: Pivot;
  readonly onColumnsChanged?: (columns: readonly Column[]) => void;
  readonly onFiltersChanged?: (filters: readonly Filter[]) => void;
  readonly onPivotChanged?: (pivot: Pivot | undefined) => void;
}

export interface Aggregator {
  readonly id: string;

  // This function is called every time the area selection changes. The purpose
  // of this function is to test whether this aggregator applies to the given
  // area selection. If it does, it returns an aggregation object which gives
  // further instructions on how to prepare the aggregation data.
  //
  // Aggregators are arranged this way because often the computation required to
  // work out whether this aggregation applies is the same as the computation
  // required to actually do the aggregation, so doing it like this means the
  // prepareData() function returned can capture intermediate state avoiding
  // having to do it again or awkwardly cache it somewhere in the aggregators
  // local state.
  probe(area: AreaSelection): Aggregation | undefined;

  // Returns the name of this aggregation tag. Called every render cycle.
  getTabName(): string;

  // Optional controls to render in the top bar of the aggregation panel.
  renderTopbarControls?(): m.Children;
}

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

export function getPresetDisplayName(preset: AggregatorGridPreset): string {
  if ('displayName' in preset && typeof preset.displayName === 'string') {
    return preset.displayName;
  }
  if ('name' in preset && typeof preset.name === 'string') {
    return preset.name;
  }
  if ('title' in preset && typeof preset.title === 'string') {
    return preset.title;
  }
  return 'Preset';
}

export function getPresetConfig(
  preset: AggregatorGridPreset,
): AggregatorGridConfig {
  if ('config' in preset && preset.config !== undefined) {
    return preset.config;
  }
  if ('configuration' in preset && preset.configuration !== undefined) {
    return (preset as unknown as {configuration: AggregatorGridConfig})
      .configuration;
  }
  return preset.config;
}

export function getActiveGridConfig(
  configOrPresets: AggregatorGridConfig | ReadonlyArray<AggregatorGridPreset>,
  presetIndex: number = 0,
): AggregatorGridConfig {
  if ('schema' in configOrPresets) {
    return configOrPresets;
  }
  if (configOrPresets.length === 0) {
    throw new Error('No presets provided');
  }
  const idx = Math.min(Math.max(0, presetIndex), configOrPresets.length - 1);
  return getPresetConfig(configOrPresets[idx]);
}

interface PreparedAggregation extends AsyncDisposable {
  readonly data: AggregationData;
  readonly dataSource: SQLDataSource;
  readonly sharedTable: SharedAsyncDisposable<DisposableSqlEntity>;
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
  const queue = new AtomicTaskQueue();
  let dataGridApi: DataGridApi | undefined;
  function createInitialState(config: AggregatorGridConfig): DataGridModel {
    return {
      columns: config.initialColumns,
      pivot: config.initialPivot,
      filters: config.initialFilters ?? [],
    };
  }

  const aggregationMemo = new Memo<Aggregation | undefined>();
  const preparedAggregationSlot = new AsyncMemo<
    PreparedAggregation | undefined
  >(queue);

  // Mutable datagrid model state - initialized the first time we get a config,
  // and only ever modified by the user so that the config is retained over
  // selection changes.
  let dataModel: DataGridModel | undefined;
  let selectedPresetIndex = 0;

  return {
    id: aggregator.id,
    name: aggregator.getTabName(),
    priority,
    render(selection: AreaSelection) {
      const selectionKey = {
        start: selection.start,
        end: selection.end,
        tracks: selection.trackUris,
      };
      const aggregation = aggregationMemo.use({
        key: selectionKey,
        compute: () => {
          const aggr = aggregator.probe(selection);
          // Snapshot the grid config if we don't have one yet
          if (aggr && !dataModel) {
            const activeConfig = getActiveGridConfig(
              aggr.getGridConfig(),
              selectedPresetIndex,
            );
            dataModel = createInitialState(activeConfig);
          }
          return aggr;
        },
      });

      if (!aggregation) {
        // The aggregation doesn't apply to this selection
        return undefined;
      }

      const configOrPresets = aggregation.getGridConfig();
      if (
        Array.isArray(configOrPresets) &&
        selectedPresetIndex >= configOrPresets.length
      ) {
        selectedPresetIndex = Math.max(0, configOrPresets.length - 1);
      }
      const activeGridConfig = getActiveGridConfig(
        configOrPresets,
        selectedPresetIndex,
      );

      const preparedAggregation = preparedAggregationSlot.use({
        key: {...selectionKey, presetIndex: selectedPresetIndex},
        compute: async () => {
          const data = await aggregation.prepareData(trace.engine);
          const sharedTable = data.sqlTable.clone();
          const sqlConfig = activeGridConfig.sqlConfig?.(data) ?? {
            tableOrSubquery: sharedTable.get().name,
          };
          const dataSource = new SQLDataSource({
            queue,
            engine: trace.engine,
            ...sqlConfig,
          });

          return {
            data,
            dataSource,
            sharedTable,
            [Symbol.asyncDispose]: async () => {
              dataSource.dispose();
              await sharedTable[Symbol.asyncDispose]();
            },
          };
        },
      }).data;

      if (!preparedAggregation) {
        // Datasource is still loading...
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

      const {data, dataSource, sharedTable} = preparedAggregation;

      // This should exist by now...
      assertExists(dataModel);

      const dataGridState: DataGridState = {
        columns: dataModel.columns,
        pivot: dataModel.pivot,
        filters: dataModel.filters,
        onColumnsChanged: (c) => {
          dataModel = {...dataModel!, columns: c};
        },
        onPivotChanged: (pivot) => {
          const isEnteringDrilldown =
            dataModel!.pivot?.drillDown === undefined &&
            pivot?.drillDown !== undefined;
          if (isEnteringDrilldown) {
            // Keep the source grid in pivot mode and open the requested
            // drill-down model in an independent tab.
            const initialDataModel: DataGridModel = {
              ...dataModel!,
              pivot,
            };
            addEphemeralTab(trace, `aggregation_drilldown_${aggregator.id}`, {
              getTitle: () => `${aggregator.getTabName()} drill-down`,
              render: () =>
                m(AggregationDrilldownPanel, {
                  trace,
                  aggregator,
                  area: selection,
                  gridConfig: activeGridConfig,
                  aggregationData: data,
                  sharedTable,
                  initialDataModel,
                }),
            });
          } else {
            dataModel = {...dataModel!, pivot};
          }
        },
        onFiltersChanged: (f) => {
          dataModel = {...dataModel!, filters: f};
        },
      };

      const presetButtons =
        Array.isArray(configOrPresets) &&
        configOrPresets.length > 0 &&
        m(
          ButtonGroup,
          configOrPresets.map((preset, index) =>
            m(Button, {
              label: getPresetDisplayName(preset),
              active: index === selectedPresetIndex,
              onclick: () => {
                if (index !== selectedPresetIndex) {
                  selectedPresetIndex = index;
                  dataModel = createInitialState(getPresetConfig(preset));
                }
              },
            }),
          ),
        );

      return {
        isLoading: false,
        content: m(AggregationPanel, {
          controls: [presetButtons, aggregator.renderTopbarControls?.()],
          key: aggregator.id,
          dataSource,
          gridConfig: activeGridConfig,
          barChartData: data?.barChartData,
          onReady: (api: DataGridApi) => {
            dataGridApi = api;
          },
          dataGridState,
          onClearGridState: () => {
            // Just wipe out the local data model to reset to initial state
            dataModel = createInitialState(activeGridConfig);
          },
        }),
        buttons:
          dataGridApi &&
          m(ExportButton, {onExportData: dataGridApi.exportData}),
      };
    },
  };
}
