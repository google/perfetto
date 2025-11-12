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
import {download} from '../base/download_utils';
import {stringifyJsonWithBigints} from '../base/json_utils';
import {Icons} from '../base/semantic_icons';
import {Duration, time, Time} from '../base/time';
import {exists} from '../base/utils';
import {
  AreaSelection,
  areaSelectionsEqual,
  AreaSelectionTab,
} from '../public/selection';
import {Trace} from '../public/trace';
import {Track} from '../public/track';
import {Dataset, DatasetSchema, UnionDataset} from '../trace_processor/dataset';
import {Engine} from '../trace_processor/engine';
import {Row, SqlValue} from '../trace_processor/query_result';
import {EmptyState} from '../widgets/empty_state';
import {Spinner} from '../widgets/spinner';
import {AggregationPanel} from './aggregation_panel';
import {DataGridDataSource} from './widgets/data_grid/common';
import {SQLDataSource} from './widgets/data_grid/sql_data_source';
import {BarChartData, ColumnDef, Sorting} from './aggregation';
import {
  createPerfettoTable,
  DisposableSqlEntity,
} from '../trace_processor/sql_utils';
import {CopyButtonHelper} from '../widgets/copy_to_clipboard_button';
import {Button} from '../widgets/button';
import {MenuItem, PopupMenu} from '../widgets/menu';
import {
  formatAsDelimited,
  formatAsMarkdownTable,
  ResponseLike,
} from './query_table/queries';

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

export interface Aggregator {
  readonly id: string;

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
  getDefaultSorting(): Sorting;
  getColumnDefinitions(): ColumnDef[];

  /**
   * Optionally override which component is used to render the data in the
   * details panel. This can be used to define customize how the data is
   * rendered.
   */
  readonly PanelComponent?: PanelComponent;
}

export interface AggregationPanelAttrs {
  readonly dataSource: DataGridDataSource;
  readonly sorting: Sorting;
  readonly columns: ReadonlyArray<ColumnDef>;
  readonly barChartData?: ReadonlyArray<BarChartData>;
}

// Define a type for the expected props of the panel components so that a
// generic AggregationPanel can be specificed as an argument to
// createBaseAggregationToTabAdaptor()
export type PanelComponent = m.ComponentTypes<AggregationPanelAttrs>;

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
  getDefaultSorting(): Sorting;
  getColumnDefinitions(): ColumnDef[];
}

export function selectTracksAndGetDataset<T extends DatasetSchema>(
  tracks: ReadonlyArray<Track>,
  spec: T,
  kind?: string,
): Dataset<T> | undefined {
  const datasets = tracks
    .filter((t) => kind === undefined || t.tags?.kinds?.includes(kind))
    .map((t) => t.renderer.getDataset?.())
    .filter(exists)
    .filter((d) => d.implements(spec));

  if (datasets.length > 0) {
    // TODO(stevegolton): Avoid typecast in UnionDataset.
    return (new UnionDataset(datasets) as unknown as Dataset<T>).optimize();
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

async function queryResponseFromAggregationData(
  aggregator: Aggregator,
  engine: Engine,
  data: AggregationData,
): Promise<ResponseLike> {
  const result = await engine.query(`SELECT * FROM ${data.tableName}`);
  const rows: Row[] = [];

  for (const iter = result.iter({}); iter.valid(); iter.next()) {
    const formattedRow: Row = {};

    for (const columnDef of aggregator.getColumnDefinitions()) {
      const value = iter.get(columnDef.columnId);
      const rendered = valueToString(value, columnDef.formatHint);
      formattedRow[columnDef.columnId] = rendered;
    }

    rows.push(formattedRow);
  }

  const columnNames: Record<string, string> = {};
  for (const columnDef of aggregator.getColumnDefinitions()) {
    columnNames[columnDef.columnId] = columnDef.title;
  }

  return {
    columns: aggregator.getColumnDefinitions().map((c) => c.columnId),
    columnNames,
    rows,
  };
}

function renderCopyButton(
  aggregator: Aggregator,
  data: AggregationData,
  trace: Trace,
  helper: CopyButtonHelper,
) {
  const label = helper.state === 'copied' ? 'Copied' : 'Copy';
  const loading = helper.state === 'working';
  const icon = helper.state === 'copied' ? Icons.Check : Icons.Copy;

  return m(
    PopupMenu,
    {
      trigger: m(Button, {
        icon,
        title: 'Copy results to clipboard',
        label,
        loading,
      }),
    },
    [
      m(MenuItem, {
        label: 'TSV',
        icon: 'tsv',
        onclick: async () => {
          const resp = await queryResponseFromAggregationData(
            aggregator,
            trace.engine,
            data,
          );
          const content = formatAsDelimited(resp);
          await helper.copy(content);
        },
      }),
      m(MenuItem, {
        label: 'Markdown',
        icon: 'table',
        onclick: async () => {
          const resp = await queryResponseFromAggregationData(
            aggregator,
            trace.engine,
            data,
          );
          const content = formatAsMarkdownTable(resp);
          await helper.copy(content);
        },
      }),
      m(MenuItem, {
        label: 'JSON',
        icon: 'data_object',
        onclick: async () => {
          const resp = await queryResponseFromAggregationData(
            aggregator,
            trace.engine,
            data,
          );
          const content = stringifyJsonWithBigints(resp.rows, 2);
          await helper.copy(content);
        },
      }),
    ],
  );
}

function renderDownloadButton(
  aggregator: Aggregator,
  data: AggregationData,
  trace: Trace,
) {
  return m(
    PopupMenu,
    {
      trigger: m(Button, {
        icon: Icons.Download,
        title: 'Download data',
        label: 'Download',
      }),
    },
    [
      m(MenuItem, {
        label: 'TSV',
        icon: 'tsv',
        onclick: async () => {
          const resp = await queryResponseFromAggregationData(
            aggregator,
            trace.engine,
            data,
          );
          const content = formatAsDelimited(resp);
          download({
            content,
            mimeType: 'text/tab-separated-values',
            fileName: 'aggregation_result.tsv',
          });
        },
      }),
      m(MenuItem, {
        label: 'Markdown',
        icon: 'table',
        onclick: async () => {
          const resp = await queryResponseFromAggregationData(
            aggregator,
            trace.engine,
            data,
          );
          const content = formatAsMarkdownTable(resp);
          download({
            content,
            mimeType: 'text/markdown',
            fileName: 'aggregation_result.md',
          });
        },
      }),
      m(MenuItem, {
        label: 'JSON',
        icon: 'data_object',
        onclick: async () => {
          const resp = await queryResponseFromAggregationData(
            aggregator,
            trace.engine,
            data,
          );
          const content = stringifyJsonWithBigints(resp.rows, 2);
          download({
            content,
            mimeType: 'text/json',
            fileName: 'aggregation_result.json',
          });
        },
      }),
    ],
  );
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
  const copyHelper = new CopyButtonHelper();
  let currentSelection: AreaSelection | undefined;
  let aggregation: Aggregation | undefined;
  let data: AggregationData | undefined;
  let dataSource: DataGridDataSource | undefined;

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
            dataSource = new SQLDataSource(trace.engine, data.tableName);
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
          sorting: aggregator.getDefaultSorting(),
          barChartData: data?.barChartData,
        }),
        buttons: data && [
          renderCopyButton(aggregator, data, trace, copyHelper),
          renderDownloadButton(aggregator, data, trace),
        ],
      };
    },
  };
}

function valueToString(value: SqlValue, formatHint?: string) {
  if (formatHint === 'DURATION_NS' && typeof value === 'bigint') {
    return Duration.humanise(value);
  } else if (formatHint === 'PERCENT' && typeof value === 'number') {
    return `${(value * 100).toFixed(2)}%`;
  } else if (value === null) {
    return 'null';
  } else if (value instanceof Uint8Array) {
    return `Blob: ${value.byteLength.toLocaleString()} bytes`;
  } else {
    return `${value}`;
  }
}
