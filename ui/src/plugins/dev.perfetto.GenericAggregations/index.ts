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

import {duration, Time, TimeSpan} from '../../base/time';
import {createAggregationToTabAdaptor} from '../../components/aggregation_adapter';
import {
  metricsFromTableOrSubquery,
  QueryFlamegraph,
} from '../../components/query_flamegraph';
import {MinimapRow} from '../../core/minimap_manager';
import {PerfettoPlugin} from '../../public/plugin';
import {AreaSelection, areaSelectionsEqual} from '../../public/selection';
import {Trace} from '../../public/trace';
import {SLICE_TRACK_KIND} from '../../public/track_kinds';
import {Engine} from '../../trace_processor/engine';
import {LONG, NUM} from '../../trace_processor/query_result';
import {Flamegraph} from '../../widgets/flamegraph';
import {CounterSelectionAggregator} from './counter_selection_aggregator';
import {PivotTableTab} from './pivot_table_tab';
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

    ctx.selection.registerAreaSelectionTab(new PivotTableTab(ctx));
    ctx.selection.registerAreaSelectionTab(createSliceFlameGraphPanel(ctx));

    ctx.minimap.registerContentProvider({
      priority: 1,
      getData: async (timeSpan, resolution) => {
        return await this.loadSliceMinimap(
          ctx.engine,
          timeSpan.toTimeSpan(),
          resolution,
        );
      },
    });
  }

  private async loadSliceMinimap(
    engine: Engine,
    traceSpan: TimeSpan,
    stepSize: duration,
  ) {
    const sliceResult = await engine.query(`
      select
        bucket,
        upid,
        ifnull(sum(utid_sum) / cast(${stepSize} as float), 0) as load
      from thread
      inner join (
        select
          ifnull(cast((ts - ${traceSpan.start})/${stepSize} as int), 0) as bucket,
          sum(dur) as utid_sum,
          utid
        from slice
        inner join thread_track on slice.track_id = thread_track.id
        group by bucket, utid
      ) using(utid)
      where upid is not null
      group by bucket, upid
    `);

    const slicesData = new Map<string, MinimapRow>();
    const it = sliceResult.iter({bucket: LONG, upid: NUM, load: NUM});
    for (; it.valid(); it.next()) {
      const bucket = it.bucket;
      const upid = it.upid;
      const load = it.load;

      const ts = Time.add(traceSpan.start, stepSize * bucket);

      const upidStr = upid.toString();
      let loadArray = slicesData.get(upidStr);
      if (loadArray === undefined) {
        loadArray = [];
        slicesData.set(upidStr, loadArray);
      }
      loadArray.push({ts, dur: stepSize, load});
    }

    const rows: MinimapRow[] = [];
    for (const row of slicesData.values()) {
      rows.push(row);
    }
    return rows;
  }
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
