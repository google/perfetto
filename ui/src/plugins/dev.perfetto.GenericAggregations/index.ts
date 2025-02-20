// Copyright (C) 2021 The Android Open Source Project
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
import {createAggregationToTabAdaptor} from '../../components/aggregation_adapter';
import {
  metricsFromTableOrSubquery,
  QueryFlamegraph,
} from '../../components/query_flamegraph';
import {PivotTableManager} from '../../components/pivot_table/pivot_table_manager';
import {PivotTable} from '../../components/pivot_table/pivot_table';
import {PerfettoPlugin} from '../../public/plugin';
import {
  AreaSelection,
  areaSelectionsEqual,
  AreaSelectionTab,
} from '../../public/selection';
import {Trace} from '../../public/trace';
import {SLICE_TRACK_KIND} from '../../public/track_kinds';
import {Flamegraph} from '../../widgets/flamegraph';
import {CounterSelectionAggregator} from './counter_selection_aggregator';
import {SliceSelectionAggregator} from './slice_selection_aggregator';

/**
 * This plugin adds the generic aggregations for slice tracks and counter
 * tracks.
 */
export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.GenericAggregations';

  async onTraceLoad(ctx: Trace): Promise<void> {
    ctx.selection.registerAreaSelectionTab(
      createAggregationToTabAdaptor(ctx, new CounterSelectionAggregator()),
    );

    ctx.selection.registerAreaSelectionTab(
      createAggregationToTabAdaptor(ctx, new SliceSelectionAggregator()),
    );

    ctx.selection.registerAreaSelectionTab(createPivotTableTab(ctx));
    ctx.selection.registerAreaSelectionTab(createSliceFlameGraphPanel(ctx));
  }
}

function createPivotTableTab(trace: Trace): AreaSelectionTab {
  // Add the pivot table and its manager
  const pivotTableMgr = new PivotTableManager(trace.engine);
  let previousSelection: undefined | AreaSelection;
  return {
    id: 'pivot_table',
    name: 'Pivot Table',
    render(selection) {
      if (
        previousSelection === undefined ||
        !areaSelectionsEqual(previousSelection, selection)
      ) {
        pivotTableMgr.setSelectionArea(selection);
        previousSelection = selection;
      }

      const pivotTableState = pivotTableMgr.state;
      const tree = pivotTableState.queryResult?.tree;

      if (
        pivotTableState.selectionArea != undefined &&
        (tree === undefined || tree.children.size > 0 || tree?.rows.length > 0)
      ) {
        return {
          isLoading: false,
          content: m(PivotTable, {
            trace,
            selectionArea: selection,
            pivotMgr: pivotTableMgr,
          }),
        };
      } else {
        return undefined;
      }
    },
  };
}

function createSliceFlameGraphPanel(trace: Trace) {
  let previousSelection: AreaSelection | undefined;
  let sliceFlamegraph: QueryFlamegraph | undefined;
  return {
    id: 'slice_flamegraph_selection',
    name: 'Slice Flamegraph',
    render(selection: AreaSelection) {
      const selectionChanged =
        previousSelection === undefined ||
        !areaSelectionsEqual(previousSelection, selection);
      previousSelection = selection;
      if (selectionChanged) {
        sliceFlamegraph = computeSliceFlamegraph(trace, selection);
      }

      if (sliceFlamegraph === undefined) {
        return undefined;
      }

      return {isLoading: false, content: sliceFlamegraph.render()};
    },
  };
}

function computeSliceFlamegraph(trace: Trace, currentSelection: AreaSelection) {
  const trackIds = [];
  for (const trackInfo of currentSelection.tracks) {
    if (trackInfo?.tags?.kind !== SLICE_TRACK_KIND) {
      continue;
    }
    if (trackInfo.tags?.trackIds === undefined) {
      continue;
    }
    trackIds.push(...trackInfo.tags.trackIds);
  }
  if (trackIds.length === 0) {
    return undefined;
  }
  const metrics = metricsFromTableOrSubquery(
    `
      (
        select *
        from _viz_slice_ancestor_agg!((
          select s.id, s.dur
          from slice s
          left join slice t on t.parent_id = s.id
          where s.ts >= ${currentSelection.start}
            and s.ts <= ${currentSelection.end}
            and s.track_id in (${trackIds.join(',')})
            and t.id is null
        ))
      )
    `,
    [
      {
        name: 'Duration',
        unit: 'ns',
        columnName: 'self_dur',
      },
      {
        name: 'Samples',
        unit: '',
        columnName: 'self_count',
      },
    ],
    'include perfetto module viz.slices;',
  );
  return new QueryFlamegraph(trace, metrics, {
    state: Flamegraph.createDefaultState(metrics),
  });
}
