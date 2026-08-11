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
import {Memo} from '../../base/memo';
import type {Store} from '../../base/store';
import {
  metricsFromTableOrSubquery,
  TreeExplorerFetcher,
  type TreeExplorerQueryMetric,
} from '../../components/tree_explorer_fetcher';
import {TreeExplorerPanel} from '../../components/tree_explorer_panel';
import type {PerfettoPlugin} from '../../public/plugin';
import {
  areaSelectionKey,
  type AreaSelection,
  type AreaSelectionTab,
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
  TREE_EXPLORER_STATE_SCHEMA,
  updateTreeExplorerState,
  type TreeExplorerState,
} from '../../widgets/tree_explorer';
import {SLICE_TRACK_KIND} from '../../public/track_kinds';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import {createCallstackTrack} from './callstack_track';
import {createProfilingTrack} from './profiling_track';
import {
  getStackSampleSourceSchema,
  type StackSampleSourceSchema,
} from './stack_sample_sources';
import {
  STACK_SAMPLE_TRACK_KIND,
  STACK_SAMPLE_FLAMECHART_TRACK_KIND,
} from './track_kinds';

export {STACK_SAMPLE_TRACK_KIND} from './track_kinds';
const LINUX_PERF_SOURCE = 'linux.perf';

const STACK_SAMPLES_PLUGIN_STATE_SCHEMA = z
  .object({
    areaSelectionFlamegraphStates: z
      .record(z.string(), TREE_EXPLORER_STATE_SCHEMA)
      .optional(),
    detailsPanelFlamegraphStates: z
      .record(z.string(), TREE_EXPLORER_STATE_SCHEMA)
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
  readonly getState: () => TreeExplorerState | undefined;
  readonly setState: (state: TreeExplorerState) => void;
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

// Appends the qualifiers which disambiguate a name (source when several
// emit, session when several exist), parenthesized: "Callstacks (Perf,
// cycles)". Empty/undefined qualifiers are dropped.
function named(base: string, ...qualifiers: (string | undefined)[]): string {
  const quals = qualifiers.filter((q) => q !== undefined && q !== '');
  return quals.length === 0 ? base : `${base} (${quals.join(', ')})`;
}

// Creates the common stack-sample track definition. Source plugins retain
// responsibility for deciding which tracks to register and where to place
// them in the workspace.
export function createStackSampleTrack(
  trace: Trace,
  uri: string,
  config: StackSampleTrackConfig,
  detailsPanelState: TreeExplorerState | undefined,
  onDetailsPanelStateChange: (state: TreeExplorerState) => void,
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
            category: NUM,
            mappingName: STR,
          },
          src: `
            select
              ss.id,
              ss.ts,
              ss.callsite_id as callsiteId,
              coalesce(mp.category, 3) as category,
              coalesce(mp.name, '') as mappingName
            from stack_sample ss
            left join stack_sample_task_context tc on tc.id = ss.task_context_id
            left join thread t on t.utid = tc.utid
            left join stack_profile_callsite c on c.id = ss.callsite_id
            left join stack_profile_frame fr on fr.id = c.frame_id
            left join _stack_sample_mapping_classification mp on mp.id = fr.mapping
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
        metricName: 'Samples',
        panelTitle: 'Callstack',
        sliceName: 'Sample',
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
  // The fetcher (and so the virtual tables built for the metrics) is created
  // for the selection it serves and disposed by the memo as soon as the
  // selection changes, so at most one generation is alive at a time.
  const fetcherMemo = new Memo<TreeExplorerFetcher | undefined>();

  return {
    id: `stack_sample_flamegraph_${encodeURIComponent(config.source)}`,
    name: named('Callstack Flamegraph', config.title),
    render: (selection: AreaSelection) => {
      const fetcher = fetcherMemo.use({
        key: areaSelectionKey(selection),
        compute: () => {
          const metrics = computeFlamegraphMetrics(selection, config);
          return metrics === undefined
            ? undefined
            : new TreeExplorerFetcher(trace, metrics);
        },
      });
      if (fetcher === undefined) return undefined;
      return {
        isLoading: false,
        content: m(TreeExplorerPanel, {
          fetcher,
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
): ReadonlyArray<TreeExplorerQueryMetric> | undefined {
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
  const metrics: TreeExplorerQueryMetric[] = [];
  for (const counterName of new Set(names)) {
    metrics.push({
      name: counterName,
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
          name: 'Sample Count',
          unit: '',
          columnName: 'self_count',
        },
      ],
      dependencySql: 'include perfetto module callstacks.stack_profile',
      ...flamegraphProperties,
      nameColumnLabel: 'Symbol',
    }),
  );

  config.setState(updateTreeExplorerState(config.getState(), metrics));
  return metrics;
}

function getScopedTrackUris(
  trace: Trace,
  source: string,
  scope: {readonly upid?: number; readonly utid?: number},
): string[] {
  return trace.tracks
    .getAllTracks()
    .filter((track) => {
      const tags = track.tags;
      const matchesScope =
        scope.utid !== undefined
          ? tags?.utid === scope.utid
          : tags?.upid !== undefined &&
            (scope.upid === undefined || tags.upid === scope.upid) &&
            tags.utid === undefined;
      return (
        tags?.kinds?.includes(STACK_SAMPLE_TRACK_KIND) === true &&
        tags.stackSampleSource === source &&
        tags.stackSampleSummary !== true &&
        !tags.kinds.includes(STACK_SAMPLE_FLAMECHART_TRACK_KIND) &&
        matchesScope
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
    if (configs.length > 0) {
      await trace.engine.query(
        'include perfetto module std.stack_sample.mapping;',
      );
    }
    const multiSource = configs.length > 1;
    for (const config of configs) {
      await this.addTracksForSource(trace, config, multiSource);
      const store = ensureExists(this.store);
      trace.selection.registerAreaSelectionTab(
        createStackSampleAreaSelectionTab(trace, {
          source: config.source,
          title: multiSource ? config.title : '',
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
            trackUris: getScopedTrackUris(trace, LINUX_PERF_SOURCE, {}),
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
    multiSource: boolean,
  ): Promise<void> {
    // With a single stack-sample source there is nothing to disambiguate;
    // only prefix track names with the source when several sources emit.
    const displayTitle = multiSource ? config.title : '';
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
    const processOnlySamples = new Set<number>();
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
        if (utid === null) {
          processOnlySamples.add(upid);
        }
      }
    }

    const sampledThreadsByUpid = new Map<number, number>();
    for (const info of byUtid.values()) {
      if (info.upid !== undefined) {
        sampledThreadsByUpid.set(
          info.upid,
          (sampledThreadsByUpid.get(info.upid) ?? 0) + 1,
        );
      }
    }

    for (const info of byUtid.values()) this.sortSessions(info.sessionIds);
    for (const info of byUpid.values()) this.sortSessions(info.sessionIds);

    const flamechartSessions = await this.queryFlamechartSessions(
      trace,
      config.source,
    );
    // Session labels disambiguate the sampling timebase, so they apply
    // whenever the trace has several kinds of sampling - be it several
    // sessions of this source or several sources - even on tracks which
    // only carry one of them.
    const labelSessions = flamechartSessions.size > 1 || multiSource;

    const groups = trace.plugins.getPlugin(ProcessThreadGroupsPlugin);
    for (const [upid, {sessionIds}] of byUpid) {
      // A process track duplicates the thread track when the process has
      // exactly one sampled thread and no process-only samples (the common
      // shape for kernel threads).
      if (
        !processOnlySamples.has(upid) &&
        (sampledThreadsByUpid.get(upid) ?? 0) === 1
      ) {
        continue;
      }
      const node = this.addScopeTracks(trace, config, {
        upid,
        utid: undefined,
        sessionIds,
        labelSessions,
        summaryName: named('Process Callstacks', displayTitle),
        leafName: (label) => named('Process Callstacks', displayTitle, label),
        uri: (sessionId) =>
          processStackSampleTrackUri(config.source, upid, sessionId),
        sortOrder: -40,
      });
      groups.getGroupForProcess(upid)?.addChildInOrder(node);
    }

    const store = ensureExists(this.store);
    const detailsPanelState = () =>
      store.state.detailsPanelFlamegraphStates?.[config.source];
    const onDetailsPanelStateChange = (state: TreeExplorerState) => {
      store.edit((draft) => {
        draft.detailsPanelFlamegraphStates ??= {};
        draft.detailsPanelFlamegraphStates[config.source] = state;
      });
    };

    // Keep sample instants on the parent, with a lazy frame-only child for
    // clock-, cycle-, or instruction-based sessions.
    const flamechartNodes: {readonly utid: number; readonly node: TrackNode}[] =
      [];
    for (const [utid, {threadName, tid, upid, sessionIds}] of byUtid) {
      const threadPrefix = `${threadName ?? 'Thread'} ${tid}`;
      const registerThreadTrack = (
        uri: string,
        sessionId: SessionId | undefined,
        name: string,
      ): TrackNode => {
        const trackConfig = {
          source: config.source,
          utid,
          upid,
          sessionId,
        };
        trace.tracks.registerTrack(
          createStackSampleTrack(
            trace,
            uri,
            trackConfig,
            detailsPanelState(),
            onDetailsPanelStateChange,
          ),
        );
        const node = new TrackNode({uri, name, sortOrder: -50});
        const supported = flamechartSessions.get(sessionId ?? null) ?? false;
        if (supported) {
          const childUri = `${uri}/flamechart`;
          trace.tracks.registerTrack(
            createCallstackTrack(trace, childUri, trackConfig),
          );
          const child = new TrackNode({
            uri: childUri,
            name: 'Callstack flamechart',
          });
          node.addChildInOrder(child);
          flamechartNodes.push({utid, node: child});
        }
        return node;
      };

      if (sessionIds.length <= 1) {
        const uri = threadStackSampleTrackUri(config.source, upid, utid);
        const sessionId = sessionIds[0];
        const sessionLabel =
          labelSessions && sessionId !== undefined && sessionId !== null
            ? this.getSessionLabel(sessionId)
            : undefined;
        const node = registerThreadTrack(
          uri,
          sessionId,
          `${threadPrefix} ${named('Callstacks', displayTitle, sessionLabel)}`,
        );
        groups.getGroupForThread(utid)?.addChildInOrder(node);
        continue;
      }

      const summaryUri = threadStackSampleTrackUri(config.source, upid, utid);
      trace.tracks.registerTrack(
        createStackSampleTrack(
          trace,
          summaryUri,
          {
            source: config.source,
            utid,
            upid,
            summary: true,
          },
          detailsPanelState(),
          onDetailsPanelStateChange,
        ),
      );
      const summaryNode = new TrackNode({
        uri: summaryUri,
        name: `${threadPrefix} ${named('Callstacks', displayTitle)}`,
        isSummary: true,
        sortOrder: -50,
      });
      for (const sessionId of sessionIds) {
        const uri = threadStackSampleTrackUri(
          config.source,
          upid,
          utid,
          sessionId,
        );
        summaryNode.addChildInOrder(
          registerThreadTrack(
            uri,
            sessionId,
            `${threadPrefix} ${named('Callstacks', displayTitle, this.sessionLabel(sessionId))}`,
          ),
        );
      }
      groups.getGroupForThread(utid)?.addChildInOrder(summaryNode);
    }

    if (flamechartNodes.length > 0) {
      // Threads with instrumented slices keep their child hidden. Other
      // threads reveal the child with compressed frame rows. These defaults
      // run once, never during pan or zoom.
      trace.onTraceReady.addListener(() => {
        const utidsWithSlices = new Set<number>();
        for (const track of trace.tracks.getAllTracks()) {
          const tags = track.tags;
          if (
            tags?.kinds?.includes(SLICE_TRACK_KIND) === true &&
            tags.utid !== undefined
          ) {
            utidsWithSlices.add(tags.utid);
          }
        }
        for (const {utid, node} of flamechartNodes) {
          if (!utidsWithSlices.has(utid)) node.reveal();
        }
      });
    }
  }

  private async queryFlamechartSessions(
    trace: Trace,
    source: string,
  ): Promise<Map<SessionId, boolean>> {
    await trace.engine.query(
      'include perfetto module std.stack_sample.flamechart;',
    );
    const result = await trace.engine.query(`
      select distinct ss.session_id as sessionId,
        _stack_sample_flamechart_supported(s.timebase_unit) as supported
      from stack_sample ss
      left join stack_sample_session s on s.id = ss.session_id
      where ss.source = ${sqlValueToSqliteString(source)}
    `);
    const sessions = new Map<SessionId, boolean>();
    for (
      const it = result.iter({sessionId: NUM_NULL, supported: NUM});
      it.valid();
      it.next()
    ) {
      sessions.set(it.sessionId, it.supported !== 0);
    }
    return sessions;
  }

  private addScopeTracks(
    trace: Trace,
    config: StackSampleSourceSchema,
    args: {
      readonly upid: number | undefined;
      readonly utid: number | undefined;
      readonly sessionIds: SessionId[];
      readonly labelSessions: boolean;
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

    // Only split into per-session tracks when this scope has several, but
    // label a lone session whenever the trace as a whole is multi-session.
    const splitBySession = args.sessionIds.length > 1;
    if (!splitBySession) {
      // Keep the merged uri but carry the lone session in the track's tags,
      // so area selection only surfaces the measures it actually has.
      const uri = args.uri();
      registerTrack(uri, args.sessionIds[0], false);
      const sessionId = args.sessionIds[0];
      const name =
        args.labelSessions && sessionId !== undefined && sessionId !== null
          ? args.leafName(this.getSessionLabel(sessionId))
          : args.summaryName;
      return new TrackNode({
        uri,
        name,
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
      const label = this.sessionLabel(sessionId);
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

  private sessionLabel(sessionId: SessionId): string {
    return sessionId === null ? 'no session' : this.getSessionLabel(sessionId);
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
    let trackUris: string[] = [];
    if (row.upid !== null) {
      trackUris = getScopedTrackUris(trace, row.source, {upid: row.upid});
    } else if (row.utid !== null) {
      trackUris = getScopedTrackUris(trace, row.source, {utid: row.utid});
    }
    if (trackUris.length === 0) return;
    trace.selection.selectArea({
      start: trace.traceInfo.start,
      end: trace.traceInfo.end,
      trackUris,
    });
  }
}
