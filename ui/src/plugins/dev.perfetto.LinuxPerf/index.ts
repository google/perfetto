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

import {assertExists} from '../../base/assert';
import {
  QueryFlamegraph,
  QueryFlamegraphMetric,
  QueryFlamegraphWithMetrics,
} from '../../components/query_flamegraph';
import {PerfettoPlugin} from '../../public/plugin';
import {AreaSelection, areaSelectionsEqual} from '../../public/selection';
import {Trace} from '../../public/trace';
import {COUNTER_TRACK_KIND} from '../../public/track_kinds';
import {getThreadUriPrefix} from '../../public/utils';
import {TrackNode} from '../../public/workspace';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {
  Flamegraph,
  FLAMEGRAPH_STATE_SCHEMA,
  FlamegraphState,
} from '../../widgets/flamegraph';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import TraceProcessorTrackPlugin from '../dev.perfetto.TraceProcessorTrack';
import {TraceProcessorCounterTrack} from '../dev.perfetto.TraceProcessorTrack/trace_processor_counter_track';
import {Store} from '../../base/store';
import {z} from 'zod';
import CpuProfilePlugin from '../dev.perfetto.CpuProfile';
import {SourceDataset} from '../../trace_processor/dataset';
import {createProfilingTrack} from '../dev.perfetto.CpuProfile/profiling_track';

const PERF_SAMPLES_PROFILE_TRACK_KIND = 'PerfSamplesProfileTrack';

const LINUX_PERF_PLUGIN_STATE_SCHEMA = z.object({
  areaSelectionFlamegraphState: FLAMEGRAPH_STATE_SCHEMA.optional(),
  detailsPanelFlamegraphState: FLAMEGRAPH_STATE_SCHEMA.optional(),
});

type LinuxPerfPluginState = z.infer<typeof LINUX_PERF_PLUGIN_STATE_SCHEMA>;

function makeUriForProc(upid: number, sessionId: number) {
  return `/process_${upid}/perf_samples_profile_${sessionId}`;
}

export default class LinuxPerfPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.LinuxPerf';
  static readonly dependencies = [
    ProcessThreadGroupsPlugin,
    TraceProcessorTrackPlugin,
    CpuProfilePlugin,
  ];

  private store?: Store<LinuxPerfPluginState>;
  // Cache of counter types per perf session, populated during onTraceLoad.
  private counterTypesBySession = new Map<number, {name: string}[]>();

  private migrateLinuxPerfPluginState(init: unknown): LinuxPerfPluginState {
    const result = LINUX_PERF_PLUGIN_STATE_SCHEMA.safeParse(init);
    return result.data ?? {};
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    this.store = trace.mountStore(LinuxPerfPlugin.id, (init) =>
      this.migrateLinuxPerfPluginState(init),
    );
    const store = assertExists(this.store);
    await this.cacheCounterTypesPerSession(trace);
    await this.addProcessPerfSamplesTracks(trace, store);
    await this.addThreadPerfSamplesTracks(trace, store);
    await this.addPerfCounterTracks(trace);

    trace.onTraceReady.addListener(async () => {
      await selectPerfTracksIfSingleProcess(trace);
    });
  }

  // Pre-fetch and cache all counter types per perf session for faster
  // flamegraph rendering.
  private async cacheCounterTypesPerSession(trace: Trace): Promise<void> {
    const result = await trace.engine.query(`
      SELECT pct.perf_session_id, pct.name, MAX(pct.is_timebase) as is_timebase
      FROM perf_counter_track pct
      GROUP BY pct.perf_session_id, pct.name
      ORDER BY pct.perf_session_id, is_timebase DESC, pct.name
    `);

    for (
      const it = result.iter({
        perf_session_id: NUM,
        name: STR_NULL,
        is_timebase: NUM_NULL,
      });
      it.valid();
      it.next()
    ) {
      const sessionId = it.perf_session_id;
      const name = it.name;
      if (name === null) continue;

      if (!this.counterTypesBySession.has(sessionId)) {
        this.counterTypesBySession.set(sessionId, []);
      }
      this.counterTypesBySession.get(sessionId)!.push({name});
    }
  }

  private async addProcessPerfSamplesTracks(
    trace: Trace,
    store: Store<LinuxPerfPluginState>,
  ) {
    const pResult = await trace.engine.query(`
      SELECT DISTINCT upid, pct.name AS cntrName, perf_session_id AS sessionId
      FROM perf_sample
      JOIN thread USING (utid)
      JOIN perf_counter_track AS pct USING (perf_session_id)
      WHERE
        callsite_id IS NOT NULL AND
        upid IS NOT NULL AND
        pct.is_timebase
      ORDER BY cntrName, perf_session_id
    `);

    // Remember all the track URIs so we can use them in a command.
    const trackUris: string[] = [];

    const countersByUpid = new Map<
      number,
      {cntrName: string; sessionId: number}[]
    >();
    for (
      const it = pResult.iter({upid: NUM, cntrName: STR, sessionId: NUM});
      it.valid();
      it.next()
    ) {
      const {upid, cntrName, sessionId} = it;
      if (!countersByUpid.has(upid)) {
        countersByUpid.set(upid, []);
      }
      countersByUpid.get(upid)!.push({cntrName, sessionId});
    }

    for (const [upid, counters] of countersByUpid) {
      // Summary track containing all callstacks, hidden if there's only one counter.
      const headless = counters.length == 1;
      const uri = `/process_${upid}/perf_samples_profile`;
      trace.tracks.registerTrack({
        uri,
        tags: {
          kinds: [PERF_SAMPLES_PROFILE_TRACK_KIND],
          upid,
        },
        renderer: createPerfCallsitesTrack(
          trace,
          uri,
          upid,
          undefined,
          undefined,
          store.state.detailsPanelFlamegraphState,
          (state) => {
            store.edit((draft) => {
              draft.detailsPanelFlamegraphState = state;
            });
          },
        ),
      });
      const group = trace.plugins
        .getPlugin(ProcessThreadGroupsPlugin)
        .getGroupForProcess(upid);
      const summaryTrack = new TrackNode({
        uri,
        name: `Process callstacks`,
        isSummary: true,
        headless: headless,
        sortOrder: -40,
      });
      group?.addChildInOrder(summaryTrack);

      // Nested tracks: one per counter being sampled on.
      for (const {cntrName, sessionId} of counters) {
        const uri = makeUriForProc(upid, sessionId);
        trackUris.push(uri);
        trace.tracks.registerTrack({
          uri,
          tags: {
            kinds: [PERF_SAMPLES_PROFILE_TRACK_KIND],
            upid,
            perfSessionId: sessionId,
          },
          renderer: createPerfCallsitesTrack(
            trace,
            uri,
            upid,
            undefined,
            sessionId,
            store.state.detailsPanelFlamegraphState,
            (state) => {
              store.edit((draft) => {
                draft.detailsPanelFlamegraphState = state;
              });
            },
          ),
        });
        const track = new TrackNode({
          uri,
          name: `Process callstacks ${cntrName}`,
          sortOrder: -40,
        });
        summaryTrack.addChildInOrder(track);
      }
    }

    // Add a command to select all the perf samples in the trace - it selects
    // the entirety of each (non-summary) process scoped perf sample track.
    trace.commands.registerCommand({
      id: 'dev.perfetto.SelectAllPerfSamples',
      name: 'Select all perf samples',
      callback: () => {
        trace.selection.selectArea({
          start: trace.traceInfo.start,
          end: trace.traceInfo.end,
          trackUris,
        });
      },
    });
  }

  private async addThreadPerfSamplesTracks(
    trace: Trace,
    store: Store<LinuxPerfPluginState>,
  ) {
    const tResult = await trace.engine.query(`
      SELECT DISTINCT
        upid, utid, tid, thread.name AS threadName,
        pct.name AS cntrName, perf_session_id AS sessionId
      FROM perf_sample
      JOIN thread USING (utid)
      JOIN perf_counter_track AS pct USING (perf_session_id)
      WHERE
        callsite_id IS NOT NULL AND
        pct.is_timebase
      ORDER BY cntrName, perf_session_id
    `);

    const countersByUtid = new Map<
      number,
      {
        threadName: string | null;
        tid: bigint;
        upid: number | null;
        cntrName: string;
        sessionId: number;
      }[]
    >();
    for (
      const it = tResult.iter({
        utid: NUM,
        tid: LONG,
        threadName: STR_NULL,
        upid: NUM_NULL,
        cntrName: STR,
        sessionId: NUM,
      });
      it.valid();
      it.next()
    ) {
      const {threadName, utid, tid, upid, cntrName, sessionId} = it;
      if (!countersByUtid.has(utid)) {
        countersByUtid.set(utid, []);
      }
      countersByUtid
        .get(utid)!
        .push({threadName, tid, upid, cntrName, sessionId});
    }

    for (const [utid, counters] of countersByUtid) {
      // Summary track containing all callstacks, hidden if there's only one counter.
      const headless = counters.length == 1;
      const tid = counters[0].tid;
      const threadName = counters[0].threadName;
      const upid = counters[0].upid;
      const uri = `${getThreadUriPrefix(upid, utid)}_perf_samples_profile`;
      trace.tracks.registerTrack({
        uri,
        tags: {
          kinds: [PERF_SAMPLES_PROFILE_TRACK_KIND],
          utid,
          upid: upid ?? undefined,
        },
        renderer: createPerfCallsitesTrack(
          trace,
          uri,
          upid ?? undefined,
          utid,
          undefined,
          store.state.detailsPanelFlamegraphState,
          (state) => {
            store.edit((draft) => {
              draft.detailsPanelFlamegraphState = state;
            });
          },
        ),
      });
      const group = trace.plugins
        .getPlugin(ProcessThreadGroupsPlugin)
        .getGroupForThread(utid);
      const summaryTrack = new TrackNode({
        uri,
        name: `${threadName ?? 'Thread'} ${tid} callstacks`,
        isSummary: true,
        headless: headless,
        sortOrder: -50,
      });
      group?.addChildInOrder(summaryTrack);

      // Nested tracks: one per counter being sampled on.
      for (const {cntrName, sessionId} of counters) {
        const uri = `${getThreadUriPrefix(upid, utid)}_perf_samples_profile_${sessionId}`;
        trace.tracks.registerTrack({
          uri,
          tags: {
            kinds: [PERF_SAMPLES_PROFILE_TRACK_KIND],
            utid,
            upid: upid ?? undefined,
            perfSessionId: sessionId,
          },
          renderer: createPerfCallsitesTrack(
            trace,
            uri,
            upid ?? undefined,
            utid,
            sessionId,
            store.state.detailsPanelFlamegraphState,
            (state) => {
              store.edit((draft) => {
                draft.detailsPanelFlamegraphState = state;
              });
            },
          ),
        });
        const track = new TrackNode({
          uri,
          name: `${threadName ?? 'Thread'} ${tid} callstacks ${cntrName}`,
          sortOrder: -50,
        });
        summaryTrack.addChildInOrder(track);
      }
    }
  }

  private async addPerfCounterTracks(trace: Trace) {
    const perfCountersGroup = new TrackNode({
      name: 'Perf counters',
      isSummary: true,
    });

    const result = await trace.engine.query(`
      select
        id,
        name,
        unit,
        cpu
      from perf_counter_track
      order by name, cpu
    `);

    const it = result.iter({
      id: NUM,
      name: STR_NULL,
      unit: STR_NULL,
      cpu: NUM_NULL,
    });

    for (; it.valid(); it.next()) {
      const {id: trackId, name, unit, cpu} = it;
      const uri = `/counter_${trackId}`;

      const title = cpu === null ? `${name}` : `Cpu ${cpu} ${name}`;
      trace.tracks.registerTrack({
        uri,
        tags: {
          kinds: [COUNTER_TRACK_KIND],
          trackIds: [trackId],
          cpu: cpu ?? undefined,
        },
        renderer: new TraceProcessorCounterTrack(
          trace,
          uri,
          {
            yMode: 'rate', // Default to rate mode
            unit: unit ?? undefined,
          },
          trackId,
          title,
        ),
      });
      const trackNode = new TrackNode({
        uri,
        name: title,
      });
      perfCountersGroup.addChildLast(trackNode);
    }

    if (perfCountersGroup.hasChildren) {
      trace.defaultWorkspace.addChildInOrder(perfCountersGroup);
    }

    trace.selection.registerAreaSelectionTab(
      this.createAreaSelectionTab(trace),
    );
  }

  private createAreaSelectionTab(trace: Trace) {
    let previousSelection: AreaSelection | undefined;
    let flamegraphWithMetrics: QueryFlamegraphWithMetrics | undefined;

    return {
      id: 'perf_sample_flamegraph',
      name: 'Perf sample flamegraph',
      render: (selection: AreaSelection) => {
        const changed =
          previousSelection === undefined ||
          !areaSelectionsEqual(previousSelection, selection);
        if (changed) {
          previousSelection = selection;
          flamegraphWithMetrics = this.computePerfSampleFlamegraph(
            trace,
            selection,
          );
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

  private computePerfSampleFlamegraph(
    trace: Trace,
    currentSelection: AreaSelection,
  ): QueryFlamegraphWithMetrics | undefined {
    const processTrackTags = getSelectedProcessTrackTags(currentSelection);
    const threadTrackTags = getSelectedThreadTrackTags(currentSelection);
    if (processTrackTags.length === 0 && threadTrackTags.length === 0) {
      return undefined;
    }

    // Get unique session IDs from selected tracks
    const sessionIds = [
      ...new Set([
        ...processTrackTags.map(([_, sessionId]) => sessionId),
        ...threadTrackTags.map(([_, sessionId]) => sessionId),
      ]),
    ];

    // Get available counter types for selected sessions from cache
    const counterTypes = this.getCounterTypesForSessions(sessionIds);

    const trackConstraints = [
      ...processTrackTags.map(
        ([upid, sessionId]) =>
          `(t.upid = ${upid} AND p.perf_session_id = ${sessionId})`,
      ),
      ...threadTrackTags.map(
        ([utid, sessionId]) =>
          `(t.utid = ${utid} AND p.perf_session_id = ${sessionId})`,
      ),
    ].join(' OR ');

    const metrics: QueryFlamegraphMetric[] = [];

    // Common properties for all metrics
    const unaggregatableProperties = [
      {name: 'mapping_name', displayName: 'Mapping'},
    ];
    const aggregatableProperties = [
      {
        name: 'source_location',
        displayName: 'Source location',
        mergeAggregation: 'ONE_OR_SUMMARY' as const,
      },
    ];

    // Add a metric for each counter type (uses weighted macro with counter values)
    for (const {name: counterName} of counterTypes) {
      metrics.push({
        name: `Perf Samples (${counterName})`,
        unit: '',
        nameColumnLabel: 'Symbol',
        dependencySql: `
          include perfetto module callstacks.stack_profile;
          include perfetto module linux.perf.counters;
        `,
        statement: `
          select
            id,
            parent_id as parentId,
            name,
            mapping_name,
            source_file || ':' || line_number as source_location,
            self_value as value
          from _callstacks_for_callsites_weighted!((
            select p.callsite_id, p.counter_value as value
            from linux_perf_sample_with_counters p
            join thread t ON p.utid = t.id
            join perf_counter_track pct ON p.track_id = pct.id
            where p.ts >= ${currentSelection.start}
              and p.ts <= ${currentSelection.end}
              and pct.name = '${counterName}'
              and (${trackConstraints})
          ))
        `,
        unaggregatableProperties,
        aggregatableProperties,
      });
    }

    // Add sample count metric (uses existing query)
    metrics.push({
      name: 'Perf Samples (Sample Count)',
      unit: '',
      nameColumnLabel: 'Symbol',
      dependencySql: 'include perfetto module linux.perf.samples',
      statement: `
        select
          id,
          parent_id as parentId,
          name,
          mapping_name,
          source_file || ':' || line_number as source_location,
          self_count as value
        from _callstacks_for_callsites!((
          select p.callsite_id
          from perf_sample p
          join thread t using (utid)
          where p.ts >= ${currentSelection.start}
            and p.ts <= ${currentSelection.end}
            and (${trackConstraints})
        ))
      `,
      unaggregatableProperties,
      aggregatableProperties,
    });

    const store = assertExists(this.store);
    store.edit((draft) => {
      draft.areaSelectionFlamegraphState = Flamegraph.updateState(
        draft.areaSelectionFlamegraphState,
        metrics,
      );
    });
    return {flamegraph: new QueryFlamegraph(trace), metrics};
  }

  // Get counter types for the given session IDs from the pre-populated cache.
  private getCounterTypesForSessions(sessionIds: number[]): {name: string}[] {
    if (sessionIds.length === 0) {
      return [];
    }

    // Collect unique counter names across all sessions, preserving order
    // (timebase counters first, then alphabetical).
    const seen = new Set<string>();
    const counterTypes: {name: string}[] = [];

    for (const sessionId of sessionIds) {
      const counters = this.counterTypesBySession.get(sessionId) ?? [];
      for (const counter of counters) {
        if (!seen.has(counter.name)) {
          seen.add(counter.name);
          counterTypes.push(counter);
        }
      }
    }
    return counterTypes;
  }
}

async function selectPerfTracksIfSingleProcess(trace: Trace) {
  const profile = await assertExists(trace.engine).query(`
    select distinct upid
    from perf_sample
    join thread using (utid)
    where callsite_id is not null
    order by ts asc
    limit 2
  `);
  if (profile.numRows() == 1) {
    trace.commands.runCommand('dev.perfetto.SelectAllPerfSamples');
  }
}

function getSelectedProcessTrackTags(currentSelection: AreaSelection) {
  const ret: number[][] = [];
  for (const trackInfo of currentSelection.tracks) {
    // process-level aggregate tracks have a upid tag but no utid tags
    if (
      trackInfo?.tags?.kinds?.includes(PERF_SAMPLES_PROFILE_TRACK_KIND) &&
      trackInfo.tags?.perfSessionId !== undefined &&
      trackInfo.tags?.utid === undefined
    ) {
      ret.push([
        assertExists(trackInfo.tags?.upid),
        Number(trackInfo.tags.perfSessionId),
      ]);
    }
  }
  return ret;
}

function getSelectedThreadTrackTags(currentSelection: AreaSelection) {
  const ret: number[][] = [];
  for (const trackInfo of currentSelection.tracks) {
    if (
      trackInfo?.tags?.kinds?.includes(PERF_SAMPLES_PROFILE_TRACK_KIND) &&
      trackInfo.tags?.perfSessionId !== undefined &&
      trackInfo.tags?.utid !== undefined
    ) {
      ret.push([trackInfo.tags?.utid, Number(trackInfo.tags.perfSessionId)]);
    }
  }
  return ret;
}

export function createPerfCallsitesTrack(
  trace: Trace,
  uri: string,
  upid: number | undefined,
  utid: number | undefined,
  sessionId: number | undefined,
  detailsPanelState: FlamegraphState | undefined,
  onDetailsPanelStateChange: (state: FlamegraphState) => void,
) {
  const constraints = [];
  if (upid !== undefined) {
    constraints.push(`(upid = ${upid})`);
  }
  if (utid !== undefined) {
    constraints.push(`(utid = ${utid})`);
  }
  if (sessionId !== undefined) {
    constraints.push(`(perf_session_id = ${sessionId})`);
  }
  const trackConstraints = constraints.join(' AND ');
  return createProfilingTrack(
    trace,
    uri,
    {
      dataset: new SourceDataset({
        schema: {
          id: NUM,
          ts: LONG,
          callsiteId: NUM,
        },
        src: `
          SELECT
            p.id,
            ts,
            callsite_id AS callsiteId,
            upid
          FROM perf_sample AS p
          JOIN thread USING (utid)
          WHERE callsite_id IS NOT NULL
            AND ${trackConstraints}
          ORDER BY ts
        `,
      }),
      callsiteQuery: (ts) => `
        SELECT ps.callsite_id
        FROM perf_sample ps
        JOIN thread t USING (utid)
        WHERE ps.ts = ${ts}
          AND ${trackConstraints}
      `,
      sqlModule: 'linux.perf.samples',
      metricName: 'Perf Samples',
      panelTitle: 'Perf sample',
      sliceName: 'Perf sample',
    },
    detailsPanelState,
    onDetailsPanelStateChange,
  );
}
