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

import {z} from 'zod';
import {ensureExists} from '../../base/assert';
import type {Store} from '../../base/store';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {COUNTER_TRACK_KIND} from '../../public/track_kinds';
import {TrackNode} from '../../public/workspace';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {
  FLAMEGRAPH_STATE_SCHEMA,
  type FlamegraphState,
} from '../../widgets/flamegraph';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import StackSamplesPlugin, {
  createStackSampleAreaSelectionTab,
  createStackSampleTrack,
  processStackSampleTrackUri,
  threadStackSampleTrackUri,
} from '../dev.perfetto.StackSamples';
import {getStackSampleSourceSchema} from '../dev.perfetto.StackSamples/stack_sample_sources';
import TraceProcessorTrackPlugin from '../dev.perfetto.TraceProcessorTrack';
import {TraceProcessorCounterTrack} from '../dev.perfetto.TraceProcessorTrack/trace_processor_counter_track';

const SOURCE = getStackSampleSourceSchema('linux.perf');

const LINUX_PERF_PLUGIN_STATE_SCHEMA = z
  .object({
    areaSelectionFlamegraphState: FLAMEGRAPH_STATE_SCHEMA.optional(),
    detailsPanelFlamegraphState: FLAMEGRAPH_STATE_SCHEMA.optional(),
  })
  .readonly();

type LinuxPerfPluginState = z.infer<typeof LINUX_PERF_PLUGIN_STATE_SCHEMA>;

export default class LinuxPerfPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.LinuxPerf';
  static readonly dependencies = [
    ProcessThreadGroupsPlugin,
    TraceProcessorTrackPlugin,
    StackSamplesPlugin,
  ];

  private store?: Store<LinuxPerfPluginState>;
  private readonly counterNamesBySession = new Map<number, string[]>();

  async onTraceLoad(trace: Trace): Promise<void> {
    this.store = trace.mountStore(LinuxPerfPlugin.id, (init) => {
      const result = LINUX_PERF_PLUGIN_STATE_SCHEMA.safeParse(init);
      return result.data ?? {};
    });
    await this.cacheCounterNamesPerSession(trace);
    const processTrackUris = await this.addProcessSampleTracks(trace);
    await this.addThreadSampleTracks(trace);

    trace.commands.registerCommand({
      id: 'dev.perfetto.SelectAllPerfSamples',
      name: 'Select all perf samples',
      callback: () => {
        trace.selection.selectArea({
          start: trace.traceInfo.start,
          end: trace.traceInfo.end,
          trackUris: processTrackUris,
        });
      },
    });

    const store = ensureExists(this.store);
    const counterNames = [
      ...new Set([...this.counterNamesBySession.values()].flat()),
    ];
    trace.selection.registerAreaSelectionTab(
      createStackSampleAreaSelectionTab(trace, {
        source: SOURCE.source,
        title: SOURCE.title,
        counterNames,
        counterNamesBySession: this.counterNamesBySession,
        getState: () => store.state.areaSelectionFlamegraphState,
        setState: (state) => {
          store.edit((draft) => {
            draft.areaSelectionFlamegraphState = state;
          });
        },
      }),
    );

    await this.addPerfCounterTracks(trace);

    // Perf has the highest automatic-selection preference. This listener is
    // registered after StackSamples' generic-source listener and deliberately
    // overrides it when perf contains exactly one process.
    trace.onTraceReady.addListener(async () => {
      await selectPerfTracksIfSingleProcess(trace);
    });
  }

  private async cacheCounterNamesPerSession(trace: Trace): Promise<void> {
    await trace.engine.query('include perfetto module viz.summary.counters;');
    const result = await trace.engine.query(`
      select
        pct.perf_session_id as sessionId,
        pct.name,
        max(pct.is_timebase) as isTimebase
      from perf_counter_track pct
      join _counter_track_summary s on pct.id = s.id
      where pct.name is not null
      group by pct.perf_session_id, pct.name
      order by pct.perf_session_id, isTimebase desc, pct.name
    `);
    for (
      const it = result.iter({sessionId: NUM, name: STR});
      it.valid();
      it.next()
    ) {
      let names = this.counterNamesBySession.get(it.sessionId);
      if (names === undefined) {
        names = [];
        this.counterNamesBySession.set(it.sessionId, names);
      }
      names.push(it.name);
    }
  }

  private async addProcessSampleTracks(trace: Trace): Promise<string[]> {
    const result = await trace.engine.query(`
      select distinct
        coalesce(ss.upid, t.upid) as upid,
        pct.name as counterName,
        ss.session_id as sessionId
      from stack_sample ss
      join thread t using (utid)
      join perf_counter_track pct on pct.perf_session_id = ss.session_id
      where ss.source = 'linux.perf'
        and coalesce(ss.upid, t.upid) is not null
        and pct.is_timebase
      order by counterName, sessionId
    `);
    const sessionsByUpid = new Map<
      number,
      Array<{counterName: string; sessionId: number}>
    >();
    for (
      const it = result.iter({
        upid: NUM,
        counterName: STR,
        sessionId: NUM,
      });
      it.valid();
      it.next()
    ) {
      let sessions = sessionsByUpid.get(it.upid);
      if (sessions === undefined) {
        sessions = [];
        sessionsByUpid.set(it.upid, sessions);
      }
      sessions.push({counterName: it.counterName, sessionId: it.sessionId});
    }

    const store = ensureExists(this.store);
    const leafTrackUris: string[] = [];
    const groups = trace.plugins.getPlugin(ProcessThreadGroupsPlugin);
    for (const [upid, sessions] of sessionsByUpid) {
      const summaryUri = processStackSampleTrackUri(SOURCE.source, upid);
      trace.tracks.registerTrack(
        createStackSampleTrack(
          trace,
          summaryUri,
          {
            source: SOURCE.source,
            title: SOURCE.title,
            upid,
            summary: true,
          },
          store.state.detailsPanelFlamegraphState,
          (state) => this.setDetailsPanelState(state),
        ),
      );
      const summaryTrack = new TrackNode({
        uri: summaryUri,
        name: 'Process callstacks',
        isSummary: true,
        headless: sessions.length === 1,
        sortOrder: -40,
      });
      groups.getGroupForProcess(upid)?.addChildInOrder(summaryTrack);

      for (const {counterName, sessionId} of sessions) {
        const uri = processStackSampleTrackUri(SOURCE.source, upid, sessionId);
        leafTrackUris.push(uri);
        trace.tracks.registerTrack(
          createStackSampleTrack(
            trace,
            uri,
            {
              source: SOURCE.source,
              title: SOURCE.title,
              upid,
              sessionId,
            },
            store.state.detailsPanelFlamegraphState,
            (state) => this.setDetailsPanelState(state),
          ),
        );
        summaryTrack.addChildInOrder(
          new TrackNode({
            uri,
            name: `Process callstacks ${counterName}`,
            sortOrder: -40,
          }),
        );
      }
    }
    return leafTrackUris;
  }

  private async addThreadSampleTracks(trace: Trace): Promise<void> {
    const result = await trace.engine.query(`
      select distinct
        coalesce(ss.upid, t.upid) as upid,
        ss.utid,
        t.tid,
        t.name as threadName,
        pct.name as counterName,
        ss.session_id as sessionId
      from stack_sample ss
      join thread t using (utid)
      join perf_counter_track pct on pct.perf_session_id = ss.session_id
      where ss.source = 'linux.perf' and pct.is_timebase
      order by counterName, sessionId
    `);
    interface Session {
      readonly upid: number | null;
      readonly tid: bigint;
      readonly threadName: string | null;
      readonly counterName: string;
      readonly sessionId: number;
    }
    const sessionsByUtid = new Map<number, Session[]>();
    for (
      const it = result.iter({
        upid: NUM_NULL,
        utid: NUM,
        tid: LONG,
        threadName: STR_NULL,
        counterName: STR,
        sessionId: NUM,
      });
      it.valid();
      it.next()
    ) {
      let sessions = sessionsByUtid.get(it.utid);
      if (sessions === undefined) {
        sessions = [];
        sessionsByUtid.set(it.utid, sessions);
      }
      sessions.push({
        upid: it.upid,
        tid: it.tid,
        threadName: it.threadName,
        counterName: it.counterName,
        sessionId: it.sessionId,
      });
    }

    const store = ensureExists(this.store);
    const groups = trace.plugins.getPlugin(ProcessThreadGroupsPlugin);
    for (const [utid, sessions] of sessionsByUtid) {
      const {upid, tid, threadName} = sessions[0];
      const title = `${threadName ?? 'Thread'} ${tid} callstacks`;
      const summaryUri = threadStackSampleTrackUri(
        SOURCE.source,
        upid ?? undefined,
        utid,
      );
      trace.tracks.registerTrack(
        createStackSampleTrack(
          trace,
          summaryUri,
          {
            source: SOURCE.source,
            title: SOURCE.title,
            upid: upid ?? undefined,
            utid,
            summary: true,
          },
          store.state.detailsPanelFlamegraphState,
          (state) => this.setDetailsPanelState(state),
        ),
      );
      const summaryTrack = new TrackNode({
        uri: summaryUri,
        name: title,
        isSummary: true,
        headless: sessions.length === 1,
        sortOrder: -50,
      });
      groups.getGroupForThread(utid)?.addChildInOrder(summaryTrack);

      for (const {counterName, sessionId} of sessions) {
        const uri = threadStackSampleTrackUri(
          SOURCE.source,
          upid ?? undefined,
          utid,
          sessionId,
        );
        trace.tracks.registerTrack(
          createStackSampleTrack(
            trace,
            uri,
            {
              source: SOURCE.source,
              title: SOURCE.title,
              upid: upid ?? undefined,
              utid,
              sessionId,
            },
            store.state.detailsPanelFlamegraphState,
            (state) => this.setDetailsPanelState(state),
          ),
        );
        summaryTrack.addChildInOrder(
          new TrackNode({uri, name: `${title} ${counterName}`, sortOrder: -50}),
        );
      }
    }
  }

  private setDetailsPanelState(state: FlamegraphState): void {
    ensureExists(this.store).edit((draft) => {
      draft.detailsPanelFlamegraphState = state;
    });
  }

  private async addPerfCounterTracks(trace: Trace): Promise<void> {
    const perfCountersGroup = new TrackNode({
      name: 'Perf counters',
      isSummary: true,
    });
    const result = await trace.engine.query(`
      select id, name, unit, cpu
      from perf_counter_track
      order by name, cpu
    `);
    for (
      const it = result.iter({
        id: NUM,
        name: STR_NULL,
        unit: STR_NULL,
        cpu: NUM_NULL,
      });
      it.valid();
      it.next()
    ) {
      const uri = `/counter_${it.id}`;
      const title = it.cpu === null ? `${it.name}` : `Cpu ${it.cpu} ${it.name}`;
      trace.tracks.registerTrack({
        uri,
        tags: {
          kinds: [COUNTER_TRACK_KIND],
          trackIds: [it.id],
          cpu: it.cpu ?? undefined,
        },
        renderer: new TraceProcessorCounterTrack({
          trace,
          uri,
          yMode: 'rate',
          unit: it.unit ?? undefined,
          trackId: it.id,
          trackName: title,
        }),
      });
      perfCountersGroup.addChildLast(new TrackNode({uri, name: title}));
    }
    if (perfCountersGroup.hasChildren) {
      trace.defaultWorkspace.addChildInOrder(perfCountersGroup);
    }
  }
}

async function selectPerfTracksIfSingleProcess(trace: Trace): Promise<void> {
  const profile = await ensureExists(trace.engine).query(`
    select distinct coalesce(ss.upid, t.upid) as upid
    from stack_sample ss
    join thread t using (utid)
    join perf_counter_track pct on pct.perf_session_id = ss.session_id
    where ss.source = 'linux.perf'
      and pct.is_timebase
      and coalesce(ss.upid, t.upid) is not null
    order by ss.ts
    limit 2
  `);
  if (profile.numRows() === 1) {
    trace.commands.runCommand('dev.perfetto.SelectAllPerfSamples');
  }
}
