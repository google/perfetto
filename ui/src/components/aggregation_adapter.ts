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
import {Time} from '../base/time';
import {exists} from '../base/utils';
import {
  AreaSelection,
  areaSelectionsEqual,
  AreaSelectionTab,
} from '../public/selection';
import {Trace} from '../public/trace';
import {Track} from '../public/track';
import {
  Dataset,
  DatasetSchema,
  SourceDataset,
  UnionDataset,
} from '../trace_processor/dataset';
import {Engine} from '../trace_processor/engine';
import {EmptyState} from '../widgets/empty_state';
import {Spinner} from '../widgets/spinner';
import {AggregateData, BarChartData, ColumnDef, Sorting} from './aggregation';
import {AggregationPanel} from './aggregation_panel';
import {SelectionAggregationManager} from './selection_aggregation_manager';

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

export interface AggState {
  getSortingPrefs(): Sorting | undefined;
  toggleSortingColumn(column: string): void;
}

export interface AggregationPanelAttrs {
  readonly trace: Trace;
  readonly data: AggregateData;
  readonly model: AggState;
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
    .filter((t) => kind === undefined || t.tags?.kind === kind)
    .map((t) => t.renderer.getDataset?.())
    .filter(exists)
    .filter((d) => d.implements(spec));

  if (datasets.length > 0) {
    // TODO(stevegolton): Avoid typecast in UnionDataset.
    return new UnionDataset(datasets) as unknown as Dataset<T>;
  } else {
    return undefined;
  }
}

export async function ii<T extends {ts: bigint; dur: bigint; id: number}>(
  engine: Engine,
  id: string,
  dataset: Dataset<T>,
  area: AreaSelection,
): Promise<Dataset<T>> {
  const duration = Time.durationBetween(area.start, area.end);

  if (duration <= 0n) {
    // Return an empty dataset if the area selection's length is zero or less.
    // II can't handle 0 or negative durations.
    return new SourceDataset({
      src: `
        SELECT * FROM (${dataset.query()})
        LIMIT 0
      `,
      schema: dataset.schema,
    });
  }

  // Materialize the source into a perfetto table first.
  // Note: the `ORDER BY id` is absolutely crucial. Removing this
  // significantly worsens aggregation results compared to no
  // materialization at all.
  const tableName = `__ii_${id}`;
  await engine.query(`
    CREATE OR REPLACE PERFETTO TABLE ${tableName} AS
    ${dataset.query()}
    ORDER BY id
  `);

  // Pass the interval intersect to the aggregator.
  await engine.query('INCLUDE PERFETTO MODULE viz.aggregation');
  const iiDataset = new SourceDataset({
    src: `
      SELECT
        ii_dur AS dur,
        *
      FROM _intersect_slices!(
        ${area.start},
        ${duration},
        ${tableName}
      )
    `,
    schema: dataset.schema,
  });

  return iiDataset;
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
  const aggMan = new SelectionAggregationManager(trace.engine, aggregator);
  let currentSelection: AreaSelection | undefined;
  let canAggregate = false;

  return {
    id: aggregator.id,
    name: aggregator.getTabName(),
    priority,
    render(selection: AreaSelection) {
      if (
        currentSelection === undefined ||
        !areaSelectionsEqual(selection, currentSelection)
      ) {
        canAggregate = aggMan.aggregateArea(selection);
        currentSelection = selection;
      }

      if (!canAggregate) {
        return undefined;
      }

      const data = aggMan.aggregatedData;
      if (!data) {
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

      return {
        isLoading: false,
        content: m(aggregator.Panel ?? AggregationPanel, {
          data,
          trace,
          model: aggMan,
        }),
      };
    },
  };
}
