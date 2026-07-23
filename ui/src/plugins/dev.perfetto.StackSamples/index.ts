// Copyright (C) 2026 The Android Open Source Project
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
import {z} from 'zod';
import {ensureExists} from '../../base/assert';
import type {Store} from '../../base/store';
import {
  metricsFromTableOrSubquery,
  type QueryFlamegraphMetric,
} from '../../components/query_flamegraph';
import {FlamegraphPanel} from '../../components/flamegraph_panel';
import type {PerfettoPlugin} from '../../public/plugin';
import {
  type AreaSelection,
  type AreaSelectionTab,
  areaSelectionsEqual,
} from '../../public/selection';
import type {Trace} from '../../public/trace';
import type {Track} from '../../public/track';
import {getThreadUriPrefix} from '../../public/utils';
import {TrackNode} from '../../public/workspace';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {SourceDataset} from '../../trace_processor/dataset';
import {sqlValueToSqliteString} from '../../trace_processor/sql_utils';
import {
  Flamegraph,
  FLAMEGRAPH_STATE_SCHEMA,
  type FlamegraphState,
} from '../../widgets/flamegraph';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import {createProfilingTrack} from './profiling_track';
import {
  getStackSampleSourceSchema,
  type StackSampleSourceSchema,
} from './stack_sample_sources';

export const STACK_SAMPLE_TRACK_KIND = 'StackSampleTrack';
const LINUX_PERF_SOURCE = 'linux.perf';

const STACK_SAMPLES_PLUGIN_STATE_SCHEMA = z
  .object({
    areaSelectionFlamegraphStates: z
      .record(z.string(), FLAMEGRAPH_STATE_SCHEMA)
      .optional(),
    detailsPanelFlamegraphStates: z
      .record(z.string(), FLAMEGRAPH_STATE_SCHEMA)
      .optional(),
  })
  .readonly();

type StackSamplesPluginState = z.infer<
  typeof STACK_SAMPLES_PLUGIN_STATE_SCHEMA
>;

type SessionId = number | null;

interface SampleGroupInfo {
  readonly threadName: string | undefined;
  readonly tid: bigint;
  readonly upid: number | undefined;
  readonly sessionIds: SessionId[];
}

export interface StackSampleTrackConfig {
  readonly source: string;
  readonly title: string;
  readonly upid?: number;
  readonly utid?: number;
  // Undefined means all sessions; null means samples without a session.
  readonly sessionId?: SessionId;
  readonly summary?: boolean;
}

export interface StackSampleAreaSelectionTabConfig {
  readonly source: string;
  readonly title: string;
  readonly counterNames: readonly string[];
  readonly counterNamesBySession: ReadonlyMap<number, readonly string[]>;
  readonly getState: () => FlamegraphState | undefined;
  readonly setState: (state: FlamegraphState) => void;
}

export function processStackSampleTrackUri(
  source: string,
  upid: number,
  sessionId?: SessionId,
): string {
  return `/process_${upid}/stack_samples_${encodeURIComponent(source)}${sessionSuffix(sessionId)}`;
}

export function threadStackSampleTrackUri(
  source: string,
  upid: number | undefined,
  utid: number,
  sessionId?: SessionId,
): string {
  return `${getThreadUriPrefix(upid ?? null, utid)}_stack_samples_${encodeURIComponent(source)}${sessionSuffix(sessionId)}`;
}

function sessionSuffix(sessionId: SessionId | undefined): string {
  if (sessionId === undefined) return '';
  return sessionId === null ? '_session_none' : `_session_${sessionId}`;
}

// Creates the common stack-sample track definition. Source plugins retain
// responsibility for deciding which tracks to register and where to place
// them in the workspace.
export function createStackSampleTrack(
  trace: Trace,
  uri: string,
  config: StackSampleTrackConfig,
  detailsPanelState: FlamegraphState | undefined,
  onDetailsPanelStateChange: (state: FlamegraphState) => void,
): Track {
  const source = sqlValueToSqliteString(config.source);
  const constraints = [`ss.source = ${source}`];
  if (config.utid !== undefined) {
    constraints.push(`tc.utid = ${config.utid}`);
  } else if (config.upid !== undefined) {
    constraints.push(`coalesce(tc.upid, t.upid) = ${config.upid}`);
  }
  if (config.sessionId === null) {
    constraints.push('ss.session_id is null');
  } else if (config.sessionId !== undefined) {
    constraints.push(`ss.session_id = ${config.sessionId}`);
  }
  const trackConstraints = constraints.join(' and ');
  return {
    uri,
    tags: {
      kinds: [STACK_SAMPLE_TRACK_KIND],
      upid: config.upid,
      utid: config.utid,
      stackSampleSource: config.source,
      ...(config.sessionId !== undefined &&
        config.sessionId !== null && {
          stackSampleSessionId: config.sessionId,
        }),
      ...(config.sessionId === null && {stackSampleNullSession: true}),
      ...(config.summary && {stackSampleSummary: true}),
    },
    renderer: createProfilingTrack(
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
            select ss.id, ss.ts, ss.callsite_id as callsiteId
            from stack_sample ss
            left join stack_sample_task_context tc on tc.id = ss.task_context_id
            left join thread t on t.utid = tc.utid
            where ${trackConstraints}
            order by ss.ts
          `,
        }),
        callsiteQuery: (ts) => `
          select ss.callsite_id
          from stack_sample ss
          left join stack_sample_task_context tc on tc.id = ss.task_context_id
          left join thread t on t.utid = tc.utid
          where ss.ts = ${ts} and ${trackConstraints}
        `,
        sqlModule: 'callstacks.stack_profile',
        metricName: `${config.title} Samples`,
        panelTitle: `${config.title} Samples`,
        sliceName: `${config.title} Sample`,
      },
      detailsPanelState,
      onDetailsPanelStateChange,
    ),
  };
}

export function createStackSampleAreaSelectionTab(
  trace: Trace,
  config: StackSampleAreaSelectionTabConfig,
): AreaSelectionTab {
  let previousSelection: AreaSelection | undefined;
  let flamegraphMetrics: ReadonlyArray<QueryFlamegraphMetric> | undefined;

  return {
    id: `stack_sample_flamegraph_${encodeURIComponent(config.source)}`,
    name: `${config.title} Sample Flamegraph`,
    render: (selection: AreaSelection) => {
      const changed =
        previousSelection === undefined ||
        !areaSelectionsEqual(previousSelection, selection);
      if (changed) {
        previousSelection = selection;
        flamegraphMetrics = computeFlamegraphMetrics(selection, config);
      }
      if (flamegraphMetrics === undefined) return undefined;
      return {
        isLoading: false,
        content: m(FlamegraphPanel, {
          trace,
          metrics: flamegraphMetrics,
          state: config.getState(),
          onStateChange: config.setState,
        }),
      };
    },
  };
}

function computeFlamegraphMetrics(
  selection: AreaSelection,
  config: StackSampleAreaSelectionTabConfig,
): ReadonlyArray<QueryFlamegraphMetric> | undefined {
  const constraints: string[] = [];
  const sessionIds = new Set<number>();
  let includesAllSessions = false;
  for (const trackInfo of selection.tracks) {
    const tags = trackInfo?.tags;
    if (
      !tags?.kinds?.includes(STACK_SAMPLE_TRACK_KIND) ||
      tags.stackSampleSource !== config.source
    ) {
      continue;
    }
    const parts = [`p.source = ${sqlValueToSqliteString(config.source)}`];
    if (tags.utid !== undefined) {
      parts.push(`tc.utid = ${tags.utid}`);
    } else if (tags.upid !== undefined) {
      parts.push(`coalesce(tc.upid, t.upid) = ${tags.upid}`);
    } else {
      continue;
    }
    if (tags.stackSampleSessionId !== undefined) {
      const sessionId = Number(tags.stackSampleSessionId);
      parts.push(`p.session_id = ${sessionId}`);
      sessionIds.add(sessionId);
    } else if (tags.stackSampleNullSession === true) {
      parts.push('p.session_id is null');
    } else {
      includesAllSessions = true;
    }
    constraints.push(`(${parts.join(' and ')})`);
  }
  if (constraints.length === 0) return undefined;

  const contextFilter = constraints.join(' or ');
  const timeFilter = `p.ts >= ${selection.start} and p.ts <= ${selection.end}`;
  const flamegraphProperties = {
    unaggregatableProperties: [{name: 'mapping_name', displayName: 'Mapping'}],
    aggregatableProperties: [
      {
        name: 'source_location',
        displayName: 'Source Location',
        mergeAggregation: 'ONE_OR_SUMMARY' as const,
      },
    ],
  };

  const names =
    includesAllSessions || sessionIds.size === 0
      ? config.counterNames
      : [...sessionIds].flatMap(
          (sessionId) => config.counterNamesBySession.get(sessionId) ?? [],
        );
  const metrics: QueryFlamegraphMetric[] = [];
  for (const counterName of new Set(names)) {
    metrics.push({
      name: `${config.title} Samples (${counterName})`,
      unit: '',
      nameColumnLabel: 'Symbol',
      dependencySql: 'include perfetto module callstacks.stack_profile;',
      statement: `
        select
          id,
          parent_id as parentId,
          name,
          mapping_name,
          source_file || ':' || line_number as source_location,
          self_value as value
        from _callstacks_for_callsites_weighted!((
          select p.callsite_id, c.value as value
          from stack_sample p
          join stack_sample_counter c on c.stack_sample_id = p.id
          join stack_sample_counter_track ct on c.track_id = ct.id
          left join stack_sample_task_context tc on tc.id = p.task_context_id
          left join thread t on t.utid = tc.utid
          where ${timeFilter}
            and ct.name = ${sqlValueToSqliteString(counterName)}
            and (${contextFilter})
        ))
      `,
      ...flamegraphProperties,
    });
  }

  metrics.push(
    ...metricsFromTableOrSubquery({
      tableOrSubquery: `
        (
          select
            id,
            parent_id as parentId,
            name,
            mapping_name,
            source_file || ':' || line_number as source_location,
            self_count
          from _callstacks_for_callsites!((
            select p.callsite_id
            from stack_sample p
            left join stack_sample_task_context tc on tc.id = p.task_context_id
            left join thread t on t.utid = tc.utid
            where ${timeFilter} and (${contextFilter})
          ))
        )
      `,
      tableMetrics: [
        {
          name: `${config.title} Samples (Sample Count)`,
          unit: '',
          columnName: 'self_count',
        },
      ],
      dependencySql: 'include perfetto module callstacks.stack_profile',
      ...flamegraphProperties,
      nameColumnLabel: 'Symbol',
    }),
  );

  config.setState(Flamegraph.updateState(config.getState(), metrics));
  return metrics;
}

function getProcessTrackUris(trace: Trace, source: string): string[] {
  return trace.tracks
    .getAllTracks()
    .filter((track) => {
      const tags = track.tags;
      return (
        tags?.kinds?.includes(STACK_SAMPLE_TRACK_KIND) === true &&
        tags.stackSampleSource === source &&
        tags.upid !== undefined &&
        tags.utid === undefined &&
        tags.stackSampleSummary !== true
      );
    })
    .map((track) => track.uri);
}

export default class StackSamplesPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.StackSamples';
  static readonly dependencies = [ProcessThreadGroupsPlugin];

  private store?: Store<StackSamplesPluginState>;
  private readonly counterNamesBySession = new Map<number, string[]>();
  private readonly counterNamesBySource = new Map<string, string[]>();

  async onTraceLoad(trace: Trace): Promise<void> {
    this.store = trace.mountStore(StackSamplesPlugin.id, (init) => {
      const result = STACK_SAMPLES_PLUGIN_STATE_SCHEMA.safeParse(init);
      return result.data ?? {};
    });
    await this.cacheCounterNames(trace);

    const result = await trace.engine.query(`
      select distinct source
      from stack_sample
      where source is not null
      order by source
    `);
    const configs: StackSampleSourceSchema[] = [];
    for (const it = result.iter({source: STR}); it.valid(); it.next()) {
      configs.push(getStackSampleSourceSchema(it.source));
    }
    configs.sort(
      (a, b) => a.order - b.order || a.source.localeCompare(b.source),
    );
    for (const config of configs) {
      await this.addTracksForSource(trace, config);
      const store = ensureExists(this.store);
      trace.selection.registerAreaSelectionTab(
        createStackSampleAreaSelectionTab(trace, {
          source: config.source,
          title: config.title,
          counterNames: this.counterNamesBySource.get(config.source) ?? [],
          counterNamesBySession: this.counterNamesBySession,
          getState: () =>
            store.state.areaSelectionFlamegraphStates?.[config.source],
          setState: (state) => {
            store.edit((draft) => {
              draft.areaSelectionFlamegraphStates ??= {};
              draft.areaSelectionFlamegraphStates[config.source] = state;
            });
          },
        }),
      );
    }

    if (configs.some((config) => config.source === LINUX_PERF_SOURCE)) {
      trace.commands.registerCommand({
        id: 'dev.perfetto.SelectAllPerfSamples',
        name: 'Select all perf samples',
        callback: () => {
          trace.selection.selectArea({
            start: trace.traceInfo.start,
            end: trace.traceInfo.end,
            trackUris: getProcessTrackUris(trace, LINUX_PERF_SOURCE),
          });
        },
      });
    }

    if (configs.length > 0) {
      trace.onTraceReady.addListener(async () => {
        const preferredOrder = configs[0].order;
        await this.autoSelectSource(
          trace,
          configs.filter((config) => config.order === preferredOrder),
        );
      });
    }
  }

  private async addTracksForSource(
    trace: Trace,
    config: StackSampleSourceSchema,
  ): Promise<void> {
    const result = await trace.engine.query(`
      select distinct
        tc.utid,
        coalesce(tc.upid, t.upid) as upid,
        t.tid,
        t.name as threadName,
        ss.session_id as sessionId
      from stack_sample ss
      join stack_sample_task_context tc on tc.id = ss.task_context_id
      left join thread t on t.utid = tc.utid
      where ss.source = ${sqlValueToSqliteString(config.source)}
        and (tc.utid is not null or tc.upid is not null)
      order by ss.session_id
    `);

    const byUtid = new Map<number, SampleGroupInfo>();
    const byUpid = new Map<number, {sessionIds: SessionId[]}>();
    for (
      const it = result.iter({
        utid: NUM_NULL,
        upid: NUM_NULL,
        tid: LONG_NULL,
        threadName: STR_NULL,
        sessionId: NUM_NULL,
      });
      it.valid();
      it.next()
    ) {
      const {utid, upid, tid, threadName, sessionId} = it;
      if (utid !== null && tid !== null) {
        let info = byUtid.get(utid);
        if (info === undefined) {
          info = {
            threadName: threadName ?? undefined,
            tid,
            upid: upid ?? undefined,
            sessionIds: [],
          };
          byUtid.set(utid, info);
        }
        if (!info.sessionIds.includes(sessionId)) {
          info.sessionIds.push(sessionId);
        }
      }
      if (upid !== null) {
        let info = byUpid.get(upid);
        if (info === undefined) {
          info = {sessionIds: []};
          byUpid.set(upid, info);
        }
        if (!info.sessionIds.includes(sessionId)) {
          info.sessionIds.push(sessionId);
        }
      }
    }

    for (const info of byUtid.values()) this.sortSessions(info.sessionIds);
    for (const info of byUpid.values()) this.sortSessions(info.sessionIds);

    const groups = trace.plugins.getPlugin(ProcessThreadGroupsPlugin);
    for (const [upid, {sessionIds}] of byUpid) {
      const node = this.addScopeTracks(trace, config, {
        upid,
        utid: undefined,
        sessionIds,
        summaryName: `${config.title} Process Callstacks`,
        leafName: (label) => `${config.title} Process Callstacks ${label}`,
        uri: (sessionId) =>
          processStackSampleTrackUri(config.source, upid, sessionId),
        sortOrder: -40,
      });
      groups.getGroupForProcess(upid)?.addChildInOrder(node);
    }

    for (const [utid, {threadName, tid, upid, sessionIds}] of byUtid) {
      const title = `${threadName ?? 'Thread'} ${tid} ${config.title} Callstacks`;
      const node = this.addScopeTracks(trace, config, {
        upid,
        utid,
        sessionIds,
        summaryName: title,
        leafName: (label) => `${title} ${label}`,
        uri: (sessionId) =>
          threadStackSampleTrackUri(config.source, upid, utid, sessionId),
        sortOrder: -50,
      });
      groups.getGroupForThread(utid)?.addChildInOrder(node);
    }
  }

  private addScopeTracks(
    trace: Trace,
    config: StackSampleSourceSchema,
    args: {
      readonly upid: number | undefined;
      readonly utid: number | undefined;
      readonly sessionIds: SessionId[];
      readonly summaryName: string;
      readonly leafName: (label: string) => string;
      readonly uri: (sessionId?: SessionId) => string;
      readonly sortOrder: number;
    },
  ): TrackNode {
    const store = ensureExists(this.store);
    const registerTrack = (
      uri: string,
      sessionId: SessionId | undefined,
      summary: boolean,
    ) => {
      trace.tracks.registerTrack(
        createStackSampleTrack(
          trace,
          uri,
          {
            source: config.source,
            title: config.title,
            upid: args.upid,
            utid: args.utid,
            sessionId,
            summary,
          },
          store.state.detailsPanelFlamegraphStates?.[config.source],
          (state) => {
            store.edit((draft) => {
              draft.detailsPanelFlamegraphStates ??= {};
              draft.detailsPanelFlamegraphStates[config.source] = state;
            });
          },
        ),
      );
    };

    const splitBySession = args.sessionIds.some((id) => id !== null);
    if (!splitBySession) {
      const uri = args.uri();
      registerTrack(uri, undefined, false);
      return new TrackNode({
        uri,
        name: args.summaryName,
        sortOrder: args.sortOrder,
      });
    }

    const summaryUri = args.uri();
    registerTrack(summaryUri, undefined, true);
    const summaryTrack = new TrackNode({
      uri: summaryUri,
      name: args.summaryName,
      isSummary: true,
      headless: args.sessionIds.length === 1,
      sortOrder: args.sortOrder,
    });
    for (const sessionId of args.sessionIds) {
      const uri = args.uri(sessionId);
      registerTrack(uri, sessionId, false);
      const label =
        sessionId === null ? 'No session' : this.getSessionLabel(sessionId);
      summaryTrack.addChildInOrder(
        new TrackNode({
          uri,
          name: args.leafName(label),
          sortOrder: args.sortOrder,
        }),
      );
    }
    return summaryTrack;
  }

  private getSessionLabel(sessionId: number): string {
    return (
      this.counterNamesBySession.get(sessionId)?.[0] ?? `Session ${sessionId}`
    );
  }

  private sortSessions(sessionIds: SessionId[]): void {
    sessionIds.sort((a, b) => {
      if (a === null) return b === null ? 0 : 1;
      if (b === null) return -1;
      const aLabel = this.getSessionLabel(a);
      const bLabel = this.getSessionLabel(b);
      return aLabel < bLabel ? -1 : aLabel > bLabel ? 1 : a - b;
    });
  }

  private async cacheCounterNames(trace: Trace): Promise<void> {
    await trace.engine.query('include perfetto module viz.summary.counters;');
    const result = await trace.engine.query(`
      select
        s.source,
        ct.session_id as sessionId,
        ct.name,
        max(ct.is_timebase) as isTimebase
      from stack_sample_counter_track ct
      join stack_sample_session s on s.id = ct.session_id
      join _counter_track_summary summary on summary.id = ct.id
      where s.source is not null and ct.name is not null
      group by s.source, ct.session_id, ct.name
      order by s.source, ct.session_id, isTimebase desc, ct.name
    `);
    for (
      const it = result.iter({
        source: STR,
        sessionId: NUM_NULL,
        name: STR,
      });
      it.valid();
      it.next()
    ) {
      let sourceNames = this.counterNamesBySource.get(it.source);
      if (sourceNames === undefined) {
        sourceNames = [];
        this.counterNamesBySource.set(it.source, sourceNames);
      }
      if (!sourceNames.includes(it.name)) sourceNames.push(it.name);
      if (it.sessionId === null) continue;
      let sessionNames = this.counterNamesBySession.get(it.sessionId);
      if (sessionNames === undefined) {
        sessionNames = [];
        this.counterNamesBySession.set(it.sessionId, sessionNames);
      }
      if (!sessionNames.includes(it.name)) sessionNames.push(it.name);
    }
  }

  private async autoSelectSource(
    trace: Trace,
    configs: readonly StackSampleSourceSchema[],
  ): Promise<void> {
    const result = await trace.engine.query(`
      select ss.source, tc.utid, coalesce(tc.upid, t.upid) as upid
      from stack_sample ss
      left join stack_sample_task_context tc on tc.id = ss.task_context_id
      left join thread t on t.utid = tc.utid
      where ss.source in (${configs
        .map((config) => sqlValueToSqliteString(config.source))
        .join(', ')})
      order by ss.ts desc
      limit 1
    `);
    if (result.numRows() !== 1) return;
    const row = result.firstRow({
      source: STR,
      utid: NUM_NULL,
      upid: NUM_NULL,
    });
    let uri: string | undefined;
    if (row.upid !== null) {
      uri = processStackSampleTrackUri(row.source, row.upid);
    } else if (row.utid !== null) {
      uri = threadStackSampleTrackUri(row.source, undefined, row.utid);
    }
    if (uri === undefined || trace.tracks.getTrack(uri) === undefined) return;
    trace.selection.selectArea({
      start: trace.traceInfo.start,
      end: trace.traceInfo.end,
      trackUris: [uri],
    });
  }
}
