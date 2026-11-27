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

import {HSLColor} from '../../base/color';
import {Time} from '../../base/time';
import {getOrCreate} from '../../base/utils';
import {createAggregationTab} from '../../components/aggregation_adapter';
import {
  getColorForSlice,
  GRAY,
  makeColorScheme,
} from '../../components/colorizer';
import {CounterTrack} from '../../components/tracks/counter_track';
import {SliceTrack} from '../../components/tracks/slice_track';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import type {TrackRenderer} from '../../public/track';
import {TrackNode} from '../../public/workspace';
import {SourceDataset} from '../../trace_processor/dataset';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import {
  CAPABILITY_RESIDENCY,
  FREEZER_RESIDENCY,
  FreezerTransitionsAggregator,
  freezerTrackUri,
  OomScoreTransitionsAggregator,
  PROCESS_STATE_RESIDENCY,
  ProcessExitsAggregator,
  ProcessStartsAggregator,
  ProcessStateTransitionsAggregator,
  processTrackUri,
  ResidencyAggregator,
  TAG_CAPABILITY_TRACK,
  TAG_COUNT_TRACK,
  TAG_FREEZER_COUNT_TRACK,
  TAG_FREEZER_TRACK,
  TAG_OOM_SCORE_TRACK,
  TAG_PROCESS_TRACK,
} from './aggregators';
import {ProcessStateDetailsPanel} from './details_panel';
import {OomScoreTrack} from './oom_score_track';
import {ProcessStateCountTrack} from './state_count_track';

const BASE_SCHEMA = {
  id: NUM,
  ts: LONG,
  dur: LONG,
  upid: NUM,
  pid: NUM,
  uid: NUM_NULL,
  user_id: NUM_NULL,
  process_name: STR_NULL,
  package_name: STR_NULL,
  version_code: NUM_NULL,
  debuggable: NUM_NULL,
  state: STR,
  state_change_reason: STR_NULL,
} as const;

const NONEXISTENT_SCHEMA = {
  ...BASE_SCHEMA,
  hosting_type: STR_NULL,
  hosting_name: STR_NULL,
  trigger_type: STR_NULL,
  bind_application_delay_ms: LONG_NULL,
  process_start_delay_ms: LONG_NULL,
} as const;

const EXITED_SCHEMA = {
  ...BASE_SCHEMA,
  exit_reason: STR_NULL,
  exit_subreason: STR_NULL,
} as const;

const PROCESS_STATE_SCHEMA = {...NONEXISTENT_SCHEMA, ...EXITED_SCHEMA} as const;

const FREEZER_SCHEMA = {
  id: NUM,
  ts: LONG,
  dur: LONG,
  upid: NUM,
  pid: NUM,
  process_name: STR_NULL,
  state: STR,
  unfreeze_reason: STR_NULL,
} as const;

const CAPABILITY_SCHEMA = {
  id: NUM,
  ts: LONG,
  dur: LONG,
  upid: NUM,
  pid: NUM,
  process_name: STR_NULL,
  capability: STR,
} as const;

const SLATE = makeColorScheme(new HSLColor([210, 18, 48]));

// The per-process tracks come in their hundreds, so keep them compact.
const COMPACT_SLICES = {sliceHeight: 12, titleSizePx: 10};

// Below every other process-scoped track (-50..30) so it appears first.
const PROCESS_GROUP_SORT_ORDER = -100;

interface ProcessTrack {
  readonly node: TrackNode;
  readonly userId?: number;
  readonly packageName?: string;
}

const GROUP_STATE_SCHEMA = {
  id: NUM,
  ts: LONG,
  dur: LONG,
  group_key: STR,
  state: STR,
} as const;

function packageGroupKey(
  packageName: string,
  userId: number | undefined,
  byUser: boolean,
): string {
  return byUser ? `${userId ?? -1}:${packageName}` : packageName;
}

function packageTrackUri(key: string, byUser: boolean): string {
  return `${ProcessState.id}#${byUser ? 'user_package' : 'package'}.${key}`;
}

function userTrackUri(userId: number): string {
  return `${ProcessState.id}#user.${userId}`;
}

// Persistent system processes are always alive.
const PERSISTENT_STATES = ['PERSISTENT', 'PERSISTENT_UI'];

function addByPackage(
  parent: TrackNode,
  processes: ReadonlyArray<ProcessTrack>,
  uriOf: (packageName: string) => string,
): void {
  const byPackage = new Map<string | undefined, ProcessTrack[]>();
  for (const p of processes) {
    getOrCreate(byPackage, p.packageName, () => []).push(p);
  }
  for (const [packageName, members] of byPackage) {
    if (packageName === undefined) {
      for (const p of members) parent.addChildLast(p.node);
      continue;
    }
    // A summary node: once expanded, its few processes show the same states.
    const node = new TrackNode({
      name: `Package: ${packageName}`,
      uri: uriOf(packageName),
      isSummary: true,
    });
    for (const p of members) node.addChildLast(p.node);
    parent.addChildLast(node);
  }
}
/**
 * Visualizes Android framework process states over the lifetime of a trace.
 *
 * A process's state timeline lives in its process group. It is also mirrored
 * under one dedicated top level group, which in long memory traces is the only
 * place most of them can appear: those processes carry no other track data,
 * and so get no process group at all.
 */
export default class ProcessState implements PerfettoPlugin {
  static readonly id = 'com.android.ProcessState';
  static readonly description =
    'Visualizes Android framework process states: per-state and freezer ' +
    'concurrency counters, and per-process state, freezer, OOM score, and ' +
    'capability timelines.';
  static readonly dependencies = [ProcessThreadGroupsPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    if (!(await this.hasProcessStateData(ctx))) {
      return;
    }

    await ctx.engine.query(`
      INCLUDE PERFETTO MODULE android.process_state;
      INCLUDE PERFETTO MODULE android.freezer;

      -- Open intervals (dur = -1) run to the end of the trace.
      CREATE PERFETTO TABLE _android_process_state_alive_concurrency AS
      FROM (
        SELECT ts, iif(dur < 0, trace_end() - ts, dur) AS dur
        FROM _android_process_state_intervals
        WHERE state NOT IN ('NONEXISTENT', 'EXITED')
      )
      |> INTERVAL FLATTEN AGGREGATE COUNT(*) AS concurrency;
    `);

    ctx.selection.registerAreaSelectionTab(
      createAggregationTab(
        ctx,
        new ResidencyAggregator(ctx, PROCESS_STATE_RESIDENCY),
      ),
    );
    ctx.selection.registerAreaSelectionTab(
      createAggregationTab(ctx, new ProcessStateTransitionsAggregator(ctx)),
    );
    ctx.selection.registerAreaSelectionTab(
      createAggregationTab(ctx, new ProcessStartsAggregator(ctx)),
    );
    ctx.selection.registerAreaSelectionTab(
      createAggregationTab(ctx, new ProcessExitsAggregator(ctx)),
    );
    ctx.selection.registerAreaSelectionTab(
      createAggregationTab(
        ctx,
        new ResidencyAggregator(ctx, FREEZER_RESIDENCY),
      ),
    );
    ctx.selection.registerAreaSelectionTab(
      createAggregationTab(ctx, new FreezerTransitionsAggregator(ctx)),
    );
    ctx.selection.registerAreaSelectionTab(
      createAggregationTab(
        ctx,
        new ResidencyAggregator(ctx, CAPABILITY_RESIDENCY),
      ),
    );
    ctx.selection.registerAreaSelectionTab(
      createAggregationTab(ctx, new OomScoreTransitionsAggregator(ctx)),
    );

    const group = new TrackNode({
      name: 'Process states',
      isSummary: true,
    });
    group.addChildLast(await this.createConcurrencyTracks(ctx));
    group.addChildLast(await this.createProcessTracks(ctx));
    ctx.defaultWorkspace.addChildInOrder(group);
  }

  // The intrinsic tables only exist if the trace processor build has the
  // android_process_state plugin, and are empty unless the trace actually
  // carries the framework's process state events.
  private async hasProcessStateData(ctx: Trace): Promise<boolean> {
    const result = await ctx.engine.tryQuery(
      `SELECT count() AS cnt FROM __intrinsic_android_process_state`,
    );
    return result.ok && result.value.firstRow({cnt: NUM}).cnt > 0;
  }

  // The alive process count as the group's own track, then one counter per
  // proc state, showing how many processes were in that state at any point in
  // time, ordered by declaration order in the proto, then one per freezer
  // state.
  private async createConcurrencyTracks(ctx: Trace): Promise<TrackNode> {
    const aliveUri = `${ProcessState.id}#count.alive`;
    ctx.tracks.registerTrack({
      uri: aliveUri,
      renderer: CounterTrack.create({
        trace: ctx,
        uri: aliveUri,
        sqlSource: `
          SELECT ts, concurrency AS value
          FROM _android_process_state_alive_concurrency
        `,
      }),
      description:
        'Total number of concurrently alive processes over time, broken down ' +
        'by process state and freezer state underneath.',
    });

    const summary = new TrackNode({
      name: 'By state',
      uri: aliveUri,
    });

    // States entered and left within the same timestamp never reach a
    // concurrency of 1, so they would only ever render a flat zero track.
    const states = await ctx.engine.query(`
      SELECT
        state,
        state_rank AS rank
      FROM _android_process_state_concurrency
      GROUP BY state
      HAVING max(concurrency) > 0
      ORDER BY rank, state
    `);

    for (const it = states.iter({state: STR}); it.valid(); it.next()) {
      const {state} = it;
      const uri = `${ProcessState.id}#count.${state}`;
      ctx.tracks.registerTrack({
        uri,
        renderer: new ProcessStateCountTrack(ctx, uri, {
          state,
          label: `in ${state}`,
          concurrencyTable: '_android_process_state_concurrency',
          intervalsTable: '_android_process_state_intervals',
          trackUri: processTrackUri,
        }),
        tags: {type: TAG_COUNT_TRACK, state},
        description: `Number of processes concurrently in ${state}.`,
      });
      summary.addChildLast(new TrackNode({uri, name: state}));
    }

    const freezerStates = await ctx.engine.query(`
      SELECT state
      FROM _android_freezer_concurrency
      GROUP BY state
      HAVING max(concurrency) > 0
      ORDER BY state
    `);

    for (const it = freezerStates.iter({state: STR}); it.valid(); it.next()) {
      const {state} = it;
      const uri = `${ProcessState.id}#count.${state}`;
      ctx.tracks.registerTrack({
        uri,
        renderer: new ProcessStateCountTrack(ctx, uri, {
          state,
          label: state.toLowerCase(),
          concurrencyTable: '_android_freezer_concurrency',
          intervalsTable: '_android_freezer_intervals',
          trackUri: freezerTrackUri,
        }),
        tags: {type: TAG_FREEZER_COUNT_TRACK, state},
        description: `Number of processes concurrently ${state.toLowerCase()}.`,
      });
      summary.addChildLast(new TrackNode({uri, name: state}));
    }

    return summary;
  }

  // One timeline per process, with its freezer, OOM score, and capability
  // timelines as children when present. Grouped by package by default, with a
  // command to also split by user.
  private async createProcessTracks(ctx: Trace): Promise<TrackNode> {
    const byProcessUri = `${ProcessState.id}#by_process`;
    // This and the user rows are not summary nodes, so their state lanes stay
    // visible when expanded.
    const byProcess = new TrackNode({name: 'By process', uri: byProcessUri});
    const groups = ctx.plugins.getPlugin(ProcessThreadGroupsPlugin);

    const processes = await ctx.engine.query(`
      SELECT
        i.upid,
        i.pid,
        i.process_name AS name,
        i.user_id AS userId,
        i.package_name AS packageName,
        p.end_ts AS endTs,
        i.upid IN (SELECT upid FROM _android_freezer_intervals)
          AS hasFreezer,
        i.upid IN (SELECT upid FROM _android_process_oom_scores)
          AS hasOomScore
      FROM _android_process_state_intervals AS i
      JOIN process AS p USING (upid)
      GROUP BY i.upid
      ORDER BY
        i.uid NULLS LAST,
        min(i.ts),
        i.pid
    `);

    const capabilitiesByUpid = new Map<number, string[]>();
    const capabilities = await ctx.engine.query(`
      SELECT upid, capability
      FROM _android_process_capability_intervals
      GROUP BY upid, capability
      ORDER BY upid, capability
    `);
    for (
      const it = capabilities.iter({upid: NUM, capability: STR});
      it.valid();
      it.next()
    ) {
      getOrCreate(capabilitiesByUpid, it.upid, () => []).push(it.capability);
    }

    const processTracks: ProcessTrack[] = [];
    for (
      const it = processes.iter({
        upid: NUM,
        pid: NUM,
        name: STR_NULL,
        userId: NUM_NULL,
        packageName: STR_NULL,
        endTs: LONG_NULL,
        hasFreezer: NUM,
        hasOomScore: NUM,
      });
      it.valid();
      it.next()
    ) {
      const {upid, pid, endTs, hasFreezer, hasOomScore} = it;
      const name = it.name ?? `${pid}`;
      const uri = processTrackUri(upid);
      const processGroup = groups.getGroupForProcess(upid);
      const overviewUri = this.registerProcessTrack(ctx, processGroup, {
        uri,
        tag: TAG_PROCESS_TRACK,
        upid,
        description: `Framework process state of ${name} (${pid}) over time.`,
        create: (trackUri, ownerUri) =>
          SliceTrack.create({
            trace: ctx,
            uri: trackUri,
            dataset: new SourceDataset({
              schema: PROCESS_STATE_SCHEMA,
              src: '_android_process_state_intervals',
              filter: {col: 'upid', eq: upid},
            }),
            sliceLayout: COMPACT_SLICES,
            sliceName: (row) => row.state,
            colorizer: (row) => {
              if (row.state === 'NONEXISTENT') return SLATE;
              if (row.state === 'EXITED') return GRAY;
              return getColorForSlice(row.state);
            },
            detailsPanel: (row) => {
              const schema =
                row.state === 'NONEXISTENT'
                  ? NONEXISTENT_SCHEMA
                  : row.state === 'EXITED'
                    ? EXITED_SCHEMA
                    : BASE_SCHEMA;
              return new ProcessStateDetailsPanel(ctx, schema, row, ownerUri);
            },
            rootTableName:
              ownerUri === undefined
                ? '_android_process_state_intervals'
                : undefined,
          }),
      });
      const overviewNode = new TrackNode({
        uri: overviewUri,
        name: `Process: ${name} | PID: ${pid}`,
      });
      const groupNode = new TrackNode({
        uri,
        name: 'Process state',
        sortOrder: PROCESS_GROUP_SORT_ORDER,
      });

      if (hasFreezer) {
        const freezerUri = freezerTrackUri(upid);
        const overviewFreezerUri = this.registerProcessTrack(
          ctx,
          processGroup,
          {
            uri: freezerUri,
            tag: TAG_FREEZER_TRACK,
            upid,
            description: `Freezer state of ${name} (${pid}) over time.`,
            create: (trackUri, ownerUri) =>
              SliceTrack.create({
                trace: ctx,
                uri: trackUri,
                dataset: new SourceDataset({
                  schema: FREEZER_SCHEMA,
                  src: '_android_freezer_intervals',
                  filter: {col: 'upid', eq: upid},
                }),
                sliceLayout: COMPACT_SLICES,
                sliceName: (row) => row.state,
                colorizer: (row) => getColorForSlice(row.state),
                detailsPanel: (row) =>
                  new ProcessStateDetailsPanel(
                    ctx,
                    FREEZER_SCHEMA,
                    row,
                    ownerUri,
                  ),
                rootTableName:
                  ownerUri === undefined
                    ? '_android_freezer_intervals'
                    : undefined,
              }),
          },
        );
        overviewNode.addChildLast(
          new TrackNode({uri: overviewFreezerUri, name: 'Freezer'}),
        );
        groupNode.addChildLast(
          new TrackNode({uri: freezerUri, name: 'Freezer'}),
        );
      }

      if (hasOomScore) {
        const oomScoreUri = `${uri}.oom_score`;
        const overviewOomScoreUri = this.registerProcessTrack(
          ctx,
          processGroup,
          {
            uri: oomScoreUri,
            tag: TAG_OOM_SCORE_TRACK,
            upid,
            description: `OOM adjustment score of ${name} (${pid}) over time.`,
            create: (trackUri) =>
              new OomScoreTrack(
                ctx,
                trackUri,
                upid,
                `${name} ${pid}`,
                Time.fromRaw(endTs ?? undefined),
              ),
          },
        );
        overviewNode.addChildLast(
          new TrackNode({uri: overviewOomScoreUri, name: 'OOM score'}),
        );
        groupNode.addChildLast(
          new TrackNode({uri: oomScoreUri, name: 'OOM score'}),
        );
      }

      const processCapabilities = capabilitiesByUpid.get(upid);
      if (processCapabilities !== undefined) {
        const capabilitiesUri = `${uri}.capabilities`;
        const overviewCapabilitiesUri = this.registerProcessTrack(
          ctx,
          processGroup,
          {
            uri: capabilitiesUri,
            tag: TAG_CAPABILITY_TRACK,
            upid,
            description: `Number of enabled capabilities of ${name} (${pid}) over time.`,
            create: (trackUri) =>
              CounterTrack.create({
                trace: ctx,
                uri: trackUri,
                sqlSource: `
                  SELECT ts, count AS value
                  FROM _android_process_capability_count
                  WHERE upid = ${upid}
                `,
              }),
          },
        );
        const overviewCapabilitiesNode = new TrackNode({
          uri: overviewCapabilitiesUri,
          name: 'Capabilities',
        });
        const groupCapabilitiesNode = new TrackNode({
          uri: capabilitiesUri,
          name: 'Capabilities',
        });

        for (const capability of processCapabilities) {
          const capabilityUri = `${uri}.capability.${capability}`;
          const overviewCapabilityUri = this.registerProcessTrack(
            ctx,
            processGroup,
            {
              uri: capabilityUri,
              tag: TAG_CAPABILITY_TRACK,
              upid,
              capability,
              description: `Intervals where ${capability} is enabled for ${name} (${pid}).`,
              create: (trackUri, ownerUri) =>
                SliceTrack.create({
                  trace: ctx,
                  uri: trackUri,
                  dataset: new SourceDataset({
                    schema: CAPABILITY_SCHEMA,
                    src: `
                      SELECT *
                      FROM _android_process_capability_intervals
                      WHERE upid = ${upid}
                    `,
                    filter: {col: 'capability', eq: capability},
                  }),
                  sliceLayout: COMPACT_SLICES,
                  sliceName: (row) => row.capability,
                  colorizer: (row) => getColorForSlice(row.capability),
                  detailsPanel: (row) =>
                    new ProcessStateDetailsPanel(
                      ctx,
                      CAPABILITY_SCHEMA,
                      row,
                      ownerUri,
                    ),
                  rootTableName:
                    ownerUri === undefined
                      ? '_android_process_capability_intervals'
                      : undefined,
                }),
            },
          );
          overviewCapabilitiesNode.addChildLast(
            new TrackNode({uri: overviewCapabilityUri, name: capability}),
          );
          groupCapabilitiesNode.addChildLast(
            new TrackNode({uri: capabilityUri, name: capability}),
          );
        }

        overviewNode.addChildLast(overviewCapabilitiesNode);
        groupNode.addChildLast(groupCapabilitiesNode);
      }

      processTracks.push({
        node: overviewNode,
        userId: it.userId ?? undefined,
        packageName: it.packageName ?? undefined,
      });
      processGroup?.addChildInOrder(groupNode);
    }

    const ready = new Map<boolean, Promise<void>>();
    const showGrouping = async (byUser: boolean) => {
      await getOrCreate(ready, byUser, async () => {
        await this.registerPackageStateTracks(ctx, processTracks, byUser);
        if (byUser) await this.registerUserStateTracks(ctx, processTracks);
      });
      for (const child of [...byProcess.children]) child.remove();
      if (!byUser) {
        addByPackage(byProcess, processTracks, (pkg) =>
          packageTrackUri(packageGroupKey(pkg, undefined, false), false),
        );
        return;
      }
      const byUserId = new Map<number | undefined, ProcessTrack[]>();
      for (const p of processTracks) {
        getOrCreate(byUserId, p.userId, () => []).push(p);
      }
      for (const [userId, members] of byUserId) {
        const uriOf = (pkg: string) =>
          packageTrackUri(packageGroupKey(pkg, userId, true), true);
        if (userId === undefined) {
          addByPackage(byProcess, members, uriOf);
          continue;
        }
        const userNode = new TrackNode({
          name: `User: ${userId}`,
          uri: userTrackUri(userId),
        });
        addByPackage(userNode, members, uriOf);
        byProcess.addChildLast(userNode);
      }
    };

    await this.registerMinStateTracks(
      ctx,
      '_android_process_state_all_states',
      `'all'`,
      'TRUE',
      [{key: 'all', uri: byProcessUri, label: 'all processes'}],
      PERSISTENT_STATES,
    );
    await showGrouping(false);
    ctx.commands.registerCommand({
      id: `${ProcessState.id}#GroupByPackage`,
      name: 'Process states: group By process by package',
      callback: () => showGrouping(false),
    });
    ctx.commands.registerCommand({
      id: `${ProcessState.id}#GroupByUserAndPackage`,
      name: 'Process states: group By process by user > package',
      callback: () => showGrouping(true),
    });

    return byProcess;
  }

  // One lane per package (optionally split by user) with the most important
  // state among its processes at each moment.
  private async registerPackageStateTracks(
    ctx: Trace,
    processes: ReadonlyArray<ProcessTrack>,
    byUser: boolean,
  ): Promise<void> {
    const packages = new Map<string, string>();
    for (const p of processes) {
      if (p.packageName === undefined) continue;
      packages.set(
        packageGroupKey(p.packageName, p.userId, byUser),
        p.packageName,
      );
    }
    const table = byUser
      ? '_android_process_state_user_package_states'
      : '_android_process_state_package_states';
    const keyExpr = byUser
      ? `coalesce(user_id, -1) || ':' || package_name`
      : 'package_name';
    await this.registerMinStateTracks(
      ctx,
      table,
      keyExpr,
      'package_name IS NOT NULL',
      Array.from(packages, ([key, packageName]) => ({
        key,
        uri: packageTrackUri(key, byUser),
        label: `the processes of ${packageName}`,
      })),
    );
  }

  private async registerUserStateTracks(
    ctx: Trace,
    processes: ReadonlyArray<ProcessTrack>,
  ): Promise<void> {
    const userIds = new Set<number>();
    for (const p of processes) {
      if (p.userId !== undefined) userIds.add(p.userId);
    }
    await this.registerMinStateTracks(
      ctx,
      '_android_process_state_user_states',
      'cast(user_id AS TEXT)',
      'user_id IS NOT NULL',
      Array.from(userIds, (userId) => ({
        key: `${userId}`,
        uri: userTrackUri(userId),
        label: `the processes of user ${userId}`,
      })),
      PERSISTENT_STATES,
    );
  }

  private async registerMinStateTracks(
    ctx: Trace,
    table: string,
    keyExpr: string,
    whereExpr: string,
    groups: ReadonlyArray<{
      readonly key: string;
      readonly uri: string;
      readonly label: string;
    }>,
    excludedStates: ReadonlyArray<string> = [],
  ): Promise<void> {
    if (groups.length === 0) return;
    const ignored = ['NONEXISTENT', 'EXITED', ...excludedStates]
      .map((state) => `'${state}'`)
      .join(', ');
    const warning =
      excludedStates.length > 0
        ? ` Ignores ${excludedStates.join(' and ')}, which would otherwise ` +
          `hide all other states.`
        : '';

    await ctx.engine.query(`
      -- Open intervals (dur = -1) run to the end of the trace.
      CREATE PERFETTO TABLE ${table}_segments AS
      FROM (
        SELECT
          ${keyExpr} AS group_key,
          ts,
          iif(dur < 0, trace_end() - ts, dur) AS dur,
          state_rank
        FROM _android_process_state_intervals
        WHERE ${whereExpr}
          AND state NOT IN (${ignored})
          AND dur != 0
      )
      |> INTERVAL FLATTEN PER group_key AGGREGATE MIN(state_rank) AS rank;

      -- FLATTEN cuts at every state change of every process: merge the
      -- contiguous segments that keep the same state.
      CREATE PERFETTO TABLE ${table} AS
      WITH
        segments AS (
          SELECT
            *,
            lag(rank) OVER w IS NOT rank OR lag(ts + dur) OVER w != ts
              AS starts_run
          FROM ${table}_segments
          WINDOW w AS (PARTITION BY group_key ORDER BY ts)
        ),
        runs AS (
          SELECT
            *,
            sum(starts_run) OVER (PARTITION BY group_key ORDER BY ts) AS run
          FROM segments
        )
      SELECT
        row_number() OVER (ORDER BY group_key, min(ts)) AS id,
        min(ts) AS ts,
        max(ts + dur) - min(ts) AS dur,
        group_key,
        r.state
      FROM runs
      JOIN _android_process_state_rank AS r USING (rank)
      GROUP BY group_key, run;
    `);

    for (const {key, uri, label} of groups) {
      ctx.tracks.registerTrack({
        uri,
        renderer: SliceTrack.create({
          trace: ctx,
          uri,
          dataset: new SourceDataset({
            schema: GROUP_STATE_SCHEMA,
            src: table,
            filter: {col: 'group_key', eq: key},
          }),
          sliceLayout: COMPACT_SLICES,
          sliceName: (row) => row.state,
          colorizer: (row) => getColorForSlice(row.state),
        }),
        description:
          `Most important framework process state among ${label}. ` +
          `Empty while none of them is alive.${warning}`,
      });
    }
  }

  // Registers a process-scoped track. The process group owns it when there is
  // one; the 'By process' overview then gets a mirror under `${uri}.overview`
  // that links back to the owner, which alone keeps rootTableName so that
  // selecting a slice by id lands on it. Returns the uri for the overview node.
  private registerProcessTrack(
    ctx: Trace,
    processGroup: TrackNode | undefined,
    track: {
      uri: string;
      tag: string;
      upid: number;
      // Set on the lane of a single capability.
      capability?: string;
      description: string;
      create: (trackUri: string, ownerUri?: string) => TrackRenderer;
    },
  ): string {
    const {uri, tag, upid, capability, description, create} = track;
    const overviewUri = processGroup !== undefined ? `${uri}.overview` : uri;
    for (const trackUri of processGroup ? [uri, overviewUri] : [uri]) {
      const isOwner = trackUri === uri;
      ctx.tracks.registerTrack({
        uri: trackUri,
        renderer: create(trackUri, isOwner ? undefined : uri),
        tags: {type: tag, upid, ...(capability !== undefined && {capability})},
        description,
      });
    }
    return overviewUri;
  }
}
