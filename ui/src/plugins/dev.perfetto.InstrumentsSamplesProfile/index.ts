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

import {TrackData} from '../../components/tracks/track_data';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR_NULL,
} from '../../trace_processor/query_result';
import {assertExists} from '../../base/logging';
import {
  createProcessInstrumentsSamplesProfileTrack,
  createThreadInstrumentsSamplesProfileTrack,
} from './instruments_samples_profile_track';
import {getThreadUriPrefix} from '../../public/utils';
import {TrackNode} from '../../public/workspace';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import {AreaSelection, areaSelectionsEqual} from '../../public/selection';
import {
  metricsFromTableOrSubquery,
  QueryFlamegraph,
  QueryFlamegraphWithMetrics,
} from '../../components/query_flamegraph';
import {Flamegraph, FLAMEGRAPH_STATE_SCHEMA} from '../../widgets/flamegraph';
import {Store} from '../../base/store';
import {z} from 'zod';

export interface Data extends TrackData {
  tsStarts: BigInt64Array;
}

const INSTRUMENTS_SAMPLES_PROFILE_TRACK_KIND = 'InstrumentsSamplesProfileTrack';

const INSTRUMENTS_SAMPLES_PROFILE_PLUGIN_STATE_SCHEMA = z.object({
  areaSelectionFlamegraphState: FLAMEGRAPH_STATE_SCHEMA.optional(),
  detailsPanelFlamegraphState: FLAMEGRAPH_STATE_SCHEMA.optional(),
});

type InstrumentsSamplesProfilePluginState = z.infer<
  typeof INSTRUMENTS_SAMPLES_PROFILE_PLUGIN_STATE_SCHEMA
>;

function makeUriForProc(upid: number) {
  return `/process_${upid}/instruments_samples_profile`;
}

export default class InstrumentsSamplesProfilePlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.InstrumentsSamplesProfile';
  static readonly dependencies = [ProcessThreadGroupsPlugin];

  private store?: Store<InstrumentsSamplesProfilePluginState>;

  private migrateInstrumentsSamplesProfilePluginState(
    init: unknown,
  ): InstrumentsSamplesProfilePluginState {
    const result =
      INSTRUMENTS_SAMPLES_PROFILE_PLUGIN_STATE_SCHEMA.safeParse(init);
    return result.data ?? {};
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    this.store = ctx.mountStore(InstrumentsSamplesProfilePlugin.id, (init) =>
      this.migrateInstrumentsSamplesProfilePluginState(init),
    );
    const pResult = await ctx.engine.query(`
      select distinct upid
      from instruments_sample
      join thread using (utid)
      where callsite_id is not null and upid is not null
    `);
    const store = assertExists(this.store);
    for (const it = pResult.iter({upid: NUM}); it.valid(); it.next()) {
      const upid = it.upid;
      const uri = makeUriForProc(upid);
      ctx.tracks.registerTrack({
        uri,
        tags: {
          kinds: [INSTRUMENTS_SAMPLES_PROFILE_TRACK_KIND],
          upid,
        },
        renderer: createProcessInstrumentsSamplesProfileTrack(
          ctx,
          uri,
          upid,
          store.state.detailsPanelFlamegraphState,
          (state) => {
            store.edit((draft) => {
              draft.detailsPanelFlamegraphState = state;
            });
          },
        ),
      });
      const group = ctx.plugins
        .getPlugin(ProcessThreadGroupsPlugin)
        .getGroupForProcess(upid);
      const track = new TrackNode({
        uri,
        name: 'Process Callstacks',
        sortOrder: -40,
      });
      group?.addChildInOrder(track);
    }
    const tResult = await ctx.engine.query(`
      select distinct
        utid,
        tid,
        thread.name as threadName,
        upid
      from instruments_sample
      join thread using (utid)
      where callsite_id is not null
    `);
    for (
      const it = tResult.iter({
        utid: NUM,
        tid: LONG,
        threadName: STR_NULL,
        upid: NUM_NULL,
      });
      it.valid();
      it.next()
    ) {
      const {threadName, utid, tid, upid} = it;
      const name =
        threadName === null
          ? `Thread Callstacks ${tid}`
          : `${threadName} Callstacks ${tid}`;
      const uri = `${getThreadUriPrefix(upid, utid)}_instruments_samples_profile`;
      ctx.tracks.registerTrack({
        uri,
        tags: {
          kinds: [INSTRUMENTS_SAMPLES_PROFILE_TRACK_KIND],
          utid,
          upid: upid ?? undefined,
        },
        renderer: createThreadInstrumentsSamplesProfileTrack(
          ctx,
          uri,
          utid,
          store.state.detailsPanelFlamegraphState,
          (state) => {
            store.edit((draft) => {
              draft.detailsPanelFlamegraphState = state;
            });
          },
        ),
      });
      const group = ctx.plugins
        .getPlugin(ProcessThreadGroupsPlugin)
        .getGroupForThread(utid);
      const track = new TrackNode({uri, name, sortOrder: -50});
      group?.addChildInOrder(track);
    }

    ctx.onTraceReady.addListener(async () => {
      await selectInstrumentsSample(ctx);
    });

    ctx.selection.registerAreaSelectionTab(this.createAreaSelectionTab(ctx));
  }

  private createAreaSelectionTab(trace: Trace) {
    let previousSelection: undefined | AreaSelection;
    let flamegraphWithMetrics: QueryFlamegraphWithMetrics | undefined;
    return {
      id: 'instruments_sample_flamegraph',
      name: 'Instruments Sample Flamegraph',
      render: (selection: AreaSelection) => {
        const changed =
          previousSelection === undefined ||
          !areaSelectionsEqual(previousSelection, selection);
        if (changed) {
          flamegraphWithMetrics = this.computeInstrumentsSampleFlamegraph(
            trace,
            selection,
          );
          previousSelection = selection;
        }
        if (flamegraphWithMetrics === undefined) {
          return undefined;
        }
        const {flamegraph, metrics} = flamegraphWithMetrics;
        const store = assertExists(this.store);
        return {
          isLoading: false,
          content: flamegraph.render({
            metrics,
            state: store.state.areaSelectionFlamegraphState,
            onStateChange: (state) => {
              store.edit((draft) => {
                draft.areaSelectionFlamegraphState = state;
              });
            },
          }),
        };
      },
    };
  }

  private computeInstrumentsSampleFlamegraph(
    trace: Trace,
    currentSelection: AreaSelection,
  ): QueryFlamegraphWithMetrics | undefined {
    const upids = getUpidsFromInstrumentsSampleAreaSelection(currentSelection);
    const utids = getUtidsFromInstrumentsSampleAreaSelection(currentSelection);
    if (utids.length === 0 && upids.length === 0) {
      return undefined;
    }
    const metrics = metricsFromTableOrSubquery(
      `
      (
        select id, parent_id as parentId, name, self_count
        from _callstacks_for_callsites!((
          select p.callsite_id
          from instruments_sample p
          join thread t using (utid)
          where p.ts >= ${currentSelection.start}
            and p.ts <= ${currentSelection.end}
            and (
              p.utid in (${utids.join(',')})
              or t.upid in (${upids.join(',')})
            )
        ))
      )
    `,
      [
        {
          name: 'Instruments Samples',
          unit: '',
          columnName: 'self_count',
        },
      ],
      'include perfetto module appleos.instruments.samples',
    );
    const store = assertExists(this.store);
    store.edit((draft) => {
      draft.areaSelectionFlamegraphState = Flamegraph.updateState(
        draft.areaSelectionFlamegraphState,
        metrics,
      );
    });
    return {flamegraph: new QueryFlamegraph(trace), metrics};
  }
}

async function selectInstrumentsSample(ctx: Trace) {
  const profile = await assertExists(ctx.engine).query(`
    select upid
    from instruments_sample
    join thread using (utid)
    where callsite_id is not null
    order by ts desc
    limit 1
  `);
  if (profile.numRows() !== 1) return;
  const row = profile.firstRow({upid: NUM});
  const upid = row.upid;

  // Create an area selection over the first process with a instruments samples track
  ctx.selection.selectArea({
    start: ctx.traceInfo.start,
    end: ctx.traceInfo.end,
    trackUris: [makeUriForProc(upid)],
  });
}

function getUpidsFromInstrumentsSampleAreaSelection(
  currentSelection: AreaSelection,
) {
  const upids = [];
  for (const trackInfo of currentSelection.tracks) {
    if (
      trackInfo?.tags?.kinds?.includes(
        INSTRUMENTS_SAMPLES_PROFILE_TRACK_KIND,
      ) &&
      trackInfo.tags?.utid === undefined
    ) {
      upids.push(assertExists(trackInfo.tags?.upid));
    }
  }
  return upids;
}

function getUtidsFromInstrumentsSampleAreaSelection(
  currentSelection: AreaSelection,
) {
  const utids = [];
  for (const trackInfo of currentSelection.tracks) {
    if (
      trackInfo?.tags?.kinds?.includes(
        INSTRUMENTS_SAMPLES_PROFILE_TRACK_KIND,
      ) &&
      trackInfo.tags?.utid !== undefined
    ) {
      utids.push(trackInfo.tags?.utid);
    }
  }
  return utids;
}
