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
import {AggregationPanel, AggregationPanelAttrs} from './aggregation_panel';
import {
  AreaSelection,
  AreaSelectionAggregator,
  areaSelectionsEqual,
  AreaSelectionTab,
} from '../public/selection';
import {Trace} from '../public/trace';
import {SelectionAggregationManager} from './selection_aggregation_manager';
import {EmptyState} from '../widgets/empty_state';
import {Spinner} from '../widgets/spinner';
import {Track} from '../public/track';
import {
  Dataset,
  DatasetSchema,
  SourceDataset,
  UnionDataset,
} from '../trace_processor/dataset';
import {exists} from '../base/utils';
import {Engine} from '../trace_processor/engine';
import {Time} from '../base/time';

// Define a type for the expected props of the panel components so that a
// generic AggregationPanel can be specificed as an argument to
// createBaseAggregationToTabAdaptor()
type PanelComponent = m.ComponentTypes<AggregationPanelAttrs>;

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
    return (new UnionDataset(datasets) as unknown as Dataset<T>).optimize();
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
export function createBaseAggregationToTabAdaptor(
  trace: Trace,
  aggregator: AreaSelectionAggregator,
  PanelComponent: PanelComponent,
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
        content: m(PanelComponent, {
          data,
          trace,
          model: aggMan,
        }),
      };
    },
  };
}

export function createAggregationToTabAdaptor(
  trace: Trace,
  aggregator: AreaSelectionAggregator,
  tabPriorityOverride?: number,
): AreaSelectionTab {
  return createBaseAggregationToTabAdaptor(
    trace,
    aggregator,
    AggregationPanel,
    tabPriorityOverride,
  );
}
