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

import {
  NUM_NULL,
  STR_NULL,
  LONG_NULL,
  NUM,
  STR,
} from '../../trace_processor/query_result';
import {Trace} from '../../public/trace';
import {COUNTER_TRACK_KIND, SLICE_TRACK_KIND} from '../../public/track_kinds';
import {PerfettoPlugin} from '../../public/plugin';
import {getThreadUriPrefix, getTrackName} from '../../public/utils';
import {CounterOptions} from '../../components/tracks/base_counter_track';
import {TraceProcessorCounterTrack} from './trace_processor_counter_track';
import {exists} from '../../base/utils';
import {TrackNode} from '../../public/workspace';
import {CounterSelectionAggregator} from './counter_selection_aggregator';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import {SliceSelectionAggregator} from './slice_selection_aggregator';
import {assertExists, assertTrue} from '../../base/logging';
import {TraceProcessorSliceTrack} from './trace_processor_slice_track';
import {removeFalsyValues} from '../../base/array_utils';

const NETWORK_TRACK_REGEX = new RegExp('^.* (Received|Transmitted)( KB)?$');
const ENTITY_RESIDENCY_REGEX = new RegExp('^Entity residency:');

type Modes = CounterOptions['yMode'];

// Sets the default 'mode' for counter tracks. If the regex matches
// then the paired mode is used. Entries are in priority order so the
// first match wins.
const COUNTER_REGEX: [RegExp, Modes][] = [
  // Power counters make more sense in rate mode since you're typically
  // interested in the slope of the graph rather than the absolute
  // value.
  [new RegExp('^power..*$'), 'rate'],
  // Same for cumulative PSI stall time counters, e.g., psi.cpu.some.
  [new RegExp('^psi..*$'), 'rate'],
  // Same for network counters.
  [NETWORK_TRACK_REGEX, 'rate'],
  // Entity residency
  [ENTITY_RESIDENCY_REGEX, 'rate'],
];

function getCounterMode(name: string): Modes | undefined {
  for (const [re, mode] of COUNTER_REGEX) {
    if (name.match(re)) {
      return mode;
    }
  }
  return undefined;
}

function getDefaultCounterOptions(name: string): Partial<CounterOptions> {
  const options: Partial<CounterOptions> = {};
  options.yMode = getCounterMode(name);

  if (name.endsWith('_pct')) {
    options.yOverrideMinimum = 0;
    options.yOverrideMaximum = 100;
    options.unit = '%';
  }

  if (name.startsWith('power.')) {
    options.yRangeSharingKey = 'power';
  }

  // TODO(stevegolton): We need to rethink how this works for virtual memory.
  // The problem is we can easily have > 10GB virtual memory which dwarfs
  // physical memory making other memory tracks difficult to read.

  // if (name.startsWith('mem.')) {
  //   options.yRangeSharingKey = 'mem';
  // }

  // All 'Entity residency: foo bar1234' tracks should share a y-axis
  // with 'Entity residency: foo baz5678' etc tracks:
  {
    const r = new RegExp('Entity residency: ([^ ]+) ');
    const m = r.exec(name);
    if (m) {
      options.yRangeSharingKey = `entity-residency-${m[1]}`;
    }
  }

  {
    const r = new RegExp('GPU .* Frequency');
    const m = r.exec(name);
    if (m) {
      options.yRangeSharingKey = 'gpu-frequency';
    }
  }

  return options;
}

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Counter';
  static readonly dependencies = [ProcessThreadGroupsPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    await this.addCounterTracks(ctx);
    await this.addGpuFrequencyTracks(ctx);
    await this.addCpuFreqLimitCounterTracks(ctx);
    await this.addCpuTimeCounterTracks(ctx);
    await this.addCpuPerfCounterTracks(ctx);
    await this.addThreadCounterTracks(ctx);
    await this.addProcessCounterTracks(ctx);

    ctx.selection.registerAreaSelectionAggregator(
      new CounterSelectionAggregator(),
    );

    const trackIdsToUris = new Map<number, string>();

    await this.addGlobalAsyncTracks(ctx, trackIdsToUris);
    await this.addProcessAsyncSliceTracks(ctx, trackIdsToUris);
    await this.addThreadAsyncSliceTracks(ctx, trackIdsToUris);

    ctx.selection.registerSqlSelectionResolver({
      sqlTableName: 'slice',
      callback: async (id: number) => {
        // Locate the track for a given id in the slice table
        const result = await ctx.engine.query(`
          select
            track_id as trackId
          from
            slice
          where slice.id = ${id}
        `);

        if (result.numRows() === 0) {
          return undefined;
        }

        const {trackId} = result.firstRow({
          trackId: NUM,
        });

        const trackUri = trackIdsToUris.get(trackId);
        if (!trackUri) {
          return undefined;
        }

        return {
          trackUri,
          eventId: id,
        };
      },
    });

    ctx.selection.registerAreaSelectionAggregator(
      new SliceSelectionAggregator(),
    );
  }

  private async addCounterTracks(ctx: Trace) {
    const result = await ctx.engine.query(`
      select name, id, unit
      from (
        select name, id, unit
        from counter_track
        join _counter_track_summary using (id)
        where is_legacy_global
        union
        select name, id, unit
        from gpu_counter_track
        join _counter_track_summary using (id)
        where name != 'gpufreq'
      )
      order by name
    `);

    // Add global or GPU counter tracks that are not bound to any pid/tid.
    const it = result.iter({
      name: STR,
      unit: STR_NULL,
      id: NUM,
    });

    for (; it.valid(); it.next()) {
      const trackId = it.id;
      const title = it.name;
      const unit = it.unit ?? undefined;

      const uri = `/counter_${trackId}`;
      ctx.tracks.registerTrack({
        uri,
        title,
        tags: {
          kind: COUNTER_TRACK_KIND,
          trackIds: [trackId],
        },
        track: new TraceProcessorCounterTrack(
          ctx,
          uri,
          {
            ...getDefaultCounterOptions(title),
            unit,
          },
          trackId,
          title,
        ),
      });
      const track = new TrackNode({uri, title});
      ctx.workspace.addChildInOrder(track);
    }
  }

  async addCpuFreqLimitCounterTracks(ctx: Trace): Promise<void> {
    const cpuFreqLimitCounterTracksSql = `
      select name, id
      from cpu_counter_track
      join _counter_track_summary using (id)
      where name glob "Cpu * Freq Limit"
      order by name asc
    `;

    this.addCpuCounterTracks(ctx, cpuFreqLimitCounterTracksSql, 'cpuFreqLimit');
  }

  async addCpuTimeCounterTracks(ctx: Trace): Promise<void> {
    const cpuTimeCounterTracksSql = `
      select name, id
      from cpu_counter_track
      join _counter_track_summary using (id)
      where name glob "cpu.times.*"
      order by name asc
    `;
    this.addCpuCounterTracks(ctx, cpuTimeCounterTracksSql, 'cpuTime');
  }

  async addCpuPerfCounterTracks(ctx: Trace): Promise<void> {
    // Perf counter tracks are bound to CPUs, follow the scheduling and
    // frequency track naming convention ("Cpu N ...").
    // Note: we might not have a track for a given cpu if no data was seen from
    // it. This might look surprising in the UI, but placeholder tracks are
    // wasteful as there's no way of collapsing global counter tracks at the
    // moment.
    const addCpuPerfCounterTracksSql = `
      select printf("Cpu %u %s", cpu, name) as name, id
      from perf_counter_track as pct
      join _counter_track_summary using (id)
      order by perf_session_id asc, pct.name asc, cpu asc
    `;
    this.addCpuCounterTracks(ctx, addCpuPerfCounterTracksSql, 'cpuPerf');
  }

  async addCpuCounterTracks(
    ctx: Trace,
    sql: string,
    scope: string,
  ): Promise<void> {
    const result = await ctx.engine.query(sql);

    const it = result.iter({
      name: STR,
      id: NUM,
    });

    for (; it.valid(); it.next()) {
      const name = it.name;
      const trackId = it.id;
      const uri = `counter.cpu.${trackId}`;
      ctx.tracks.registerTrack({
        uri,
        title: name,
        tags: {
          kind: COUNTER_TRACK_KIND,
          trackIds: [trackId],
          scope,
        },
        track: new TraceProcessorCounterTrack(
          ctx,
          uri,
          getDefaultCounterOptions(name),
          trackId,
          name,
        ),
      });
      const trackNode = new TrackNode({uri, title: name, sortOrder: -20});
      ctx.workspace.addChildInOrder(trackNode);
    }
  }

  async addThreadCounterTracks(ctx: Trace): Promise<void> {
    const result = await ctx.engine.query(`
      select
        thread_counter_track.name as trackName,
        utid,
        upid,
        tid,
        thread.name as threadName,
        thread_counter_track.id as trackId,
        thread.start_ts as startTs,
        thread.end_ts as endTs
      from thread_counter_track
      join _counter_track_summary using (id)
      join thread using(utid)
      where thread_counter_track.name != 'thread_time'
    `);

    const it = result.iter({
      startTs: LONG_NULL,
      trackId: NUM,
      endTs: LONG_NULL,
      trackName: STR_NULL,
      utid: NUM,
      upid: NUM_NULL,
      tid: NUM_NULL,
      threadName: STR_NULL,
    });
    for (; it.valid(); it.next()) {
      const utid = it.utid;
      const upid = it.upid;
      const tid = it.tid;
      const trackId = it.trackId;
      const trackName = it.trackName;
      const threadName = it.threadName;
      const kind = COUNTER_TRACK_KIND;
      const name = getTrackName({
        name: trackName,
        utid,
        tid,
        kind,
        threadName,
        threadTrack: true,
      });
      const uri = `${getThreadUriPrefix(upid, utid)}_counter_${trackId}`;
      ctx.tracks.registerTrack({
        uri,
        title: name,
        tags: {
          kind,
          trackIds: [trackId],
          utid,
          upid: upid ?? undefined,
          scope: 'thread',
        },
        track: new TraceProcessorCounterTrack(
          ctx,
          uri,
          getDefaultCounterOptions(name),
          trackId,
          name,
        ),
      });
      const group = ctx.plugins
        .getPlugin(ProcessThreadGroupsPlugin)
        .getGroupForThread(utid);
      const track = new TrackNode({uri, title: name, sortOrder: 30});
      group?.addChildInOrder(track);
    }
  }

  async addProcessCounterTracks(ctx: Trace): Promise<void> {
    const result = await ctx.engine.query(`
      select
        process_counter_track.id as trackId,
        process_counter_track.name as trackName,
        upid,
        process.pid,
        process.name as processName
      from process_counter_track
      join _counter_track_summary using (id)
      join process using(upid)
      order by trackName;
    `);
    const it = result.iter({
      trackId: NUM,
      trackName: STR_NULL,
      upid: NUM,
      pid: NUM_NULL,
      processName: STR_NULL,
    });
    for (let i = 0; it.valid(); ++i, it.next()) {
      const trackId = it.trackId;
      const pid = it.pid;
      const trackName = it.trackName;
      const upid = it.upid;
      const processName = it.processName;
      const kind = COUNTER_TRACK_KIND;
      const name = getTrackName({
        name: trackName,
        upid,
        pid,
        kind,
        processName,
        ...(exists(trackName) && {trackName}),
      });
      const uri = `/process_${upid}/counter_${trackId}`;
      ctx.tracks.registerTrack({
        uri,
        title: name,
        tags: {
          kind,
          trackIds: [trackId],
          upid,
          scope: 'process',
        },
        track: new TraceProcessorCounterTrack(
          ctx,
          uri,
          getDefaultCounterOptions(name),
          trackId,
          name,
        ),
      });
      const group = ctx.plugins
        .getPlugin(ProcessThreadGroupsPlugin)
        .getGroupForProcess(upid);
      const track = new TrackNode({uri, title: name, sortOrder: 20});
      group?.addChildInOrder(track);
    }
  }

  private async addGpuFrequencyTracks(ctx: Trace) {
    const engine = ctx.engine;

    const result = await engine.query(`
      select id, gpu_id as gpuId
      from gpu_counter_track
      join _counter_track_summary using (id)
      where name = 'gpufreq'
    `);
    const it = result.iter({id: NUM, gpuId: NUM});
    for (; it.valid(); it.next()) {
      const uri = `/gpu_frequency_${it.gpuId}`;
      const name = `Gpu ${it.gpuId} Frequency`;
      ctx.tracks.registerTrack({
        uri,
        title: name,
        tags: {
          kind: COUNTER_TRACK_KIND,
          trackIds: [it.id],
          scope: 'gpuFreq',
        },
        track: new TraceProcessorCounterTrack(
          ctx,
          uri,
          getDefaultCounterOptions(name),
          it.id,
          name,
        ),
      });
      const track = new TrackNode({uri, title: name, sortOrder: -20});
      ctx.workspace.addChildInOrder(track);
    }
  }

  async addGlobalAsyncTracks(
    ctx: Trace,
    trackIdsToUris: Map<number, string>,
  ): Promise<void> {
    const {engine} = ctx;
    const rawGlobalAsyncTracks = await engine.query(`
      include perfetto module graphs.search;
      include perfetto module viz.summary.tracks;

      with global_tracks_grouped as (
        select
          t.parent_id,
          t.name,
          group_concat(t.id) as trackIds,
          count() as trackCount,
          ifnull(min(a.order_id), 0) as order_id
        from track t
        join _slice_track_summary s using (id)
        left join _track_event_tracks_ordered a USING (id)
        where
          s.is_legacy_global
          and (name != 'Suspend/Resume Latency' or name is null)
        group by parent_id, name
        order by parent_id, order_id
      ),
      intermediate_groups as (
        select
          t.name,
          t.id,
          t.parent_id,
          ifnull(a.order_id, 0) as order_id
        from graph_reachable_dfs!(
          (
            select id as source_node_id, parent_id as dest_node_id
            from track
            where parent_id is not null
          ),
          (
            select distinct parent_id as node_id
            from global_tracks_grouped
            where parent_id is not null
          )
        ) g
        join track t on g.node_id = t.id
        left join _track_event_tracks_ordered a USING (id)
      )
      select
        t.name as name,
        t.parent_id as parentId,
        t.trackIds as trackIds,
        t.order_id as orderId,
        __max_layout_depth(t.trackCount, t.trackIds) as maxDepth
      from global_tracks_grouped t
      union all
      select
        t.name as name,
        t.parent_id as parentId,
        cast_string!(t.id) as trackIds,
        t.order_id as orderId,
        NULL as maxDepth
      from intermediate_groups t
      left join _slice_track_summary s using (id)
      where s.id is null
      order by parentId, orderId
    `);
    const it = rawGlobalAsyncTracks.iter({
      name: STR_NULL,
      parentId: NUM_NULL,
      trackIds: STR,
      orderId: NUM,
      maxDepth: NUM_NULL,
    });

    // Create a map of track nodes by id
    const trackMap = new Map<
      number,
      {parentId: number | null; trackNode: TrackNode}
    >();

    for (; it.valid(); it.next()) {
      const rawName = it.name === null ? undefined : it.name;
      const title = getTrackName({
        name: rawName,
        kind: SLICE_TRACK_KIND,
      });
      const rawTrackIds = it.trackIds;
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const maxDepth = it.maxDepth;

      if (maxDepth === null) {
        assertTrue(trackIds.length == 1);
        const trackNode = new TrackNode({title, sortOrder: -25});
        trackMap.set(trackIds[0], {parentId: it.parentId, trackNode});
      } else {
        const uri = `/async_slices_${rawName}_${it.parentId}`;
        ctx.tracks.registerTrack({
          uri,
          title,
          tags: {
            trackIds,
            kind: SLICE_TRACK_KIND,
            scope: 'global',
          },
          track: new TraceProcessorSliceTrack(ctx, uri, maxDepth, trackIds),
        });
        const trackNode = new TrackNode({
          uri,
          title,
          sortOrder: it.orderId,
        });
        trackIds.forEach((id) => {
          trackMap.set(id, {parentId: it.parentId, trackNode});
          trackIdsToUris.set(id, uri);
        });
      }
    }

    // Attach track nodes to parents / or the workspace if they have no parent
    trackMap.forEach(({parentId, trackNode}) => {
      if (exists(parentId)) {
        const parent = assertExists(trackMap.get(parentId));
        parent.trackNode.addChildInOrder(trackNode);
      } else {
        ctx.workspace.addChildInOrder(trackNode);
      }
    });
  }

  async addCpuTracks(
    ctx: Trace,
    trackIdsToUris: Map<number, string>,
  ): Promise<void> {
    const {engine} = ctx;
    const res = await engine.query(`
      include perfetto module viz.summary.tracks;

      with global_tracks_grouped as (
        select
          t.name,
          group_concat(t.id) as trackIds,
          count() as trackCount
        from cpu_track t
        join _slice_track_summary using (id)
        group by name
      )
      select
        t.name as name,
        t.trackIds as trackIds,
        __max_layout_depth(t.trackCount, t.trackIds) as maxDepth
      from global_tracks_grouped t
    `);
    const it = res.iter({
      name: STR_NULL,
      trackIds: STR,
      maxDepth: NUM,
    });

    for (; it.valid(); it.next()) {
      const rawName = it.name === null ? undefined : it.name;
      const title = getTrackName({
        name: rawName,
        kind: SLICE_TRACK_KIND,
      });
      const rawTrackIds = it.trackIds;
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const maxDepth = it.maxDepth;

      const uri = `/cpu_slices_${rawName}`;
      ctx.tracks.registerTrack({
        uri,
        title,
        tags: {
          trackIds,
          kind: SLICE_TRACK_KIND,
          scope: 'global',
        },
        track: new TraceProcessorSliceTrack(ctx, uri, maxDepth, trackIds),
      });
      const trackNode = new TrackNode({
        uri,
        title,
      });
      ctx.workspace.addChildInOrder(trackNode);
      trackIds.forEach((id) => {
        trackIdsToUris.set(id, uri);
      });
    }
  }

  async addProcessAsyncSliceTracks(
    ctx: Trace,
    trackIdsToUris: Map<number, string>,
  ): Promise<void> {
    const result = await ctx.engine.query(`
      select
        upid,
        t.name as trackName,
        t.track_ids as trackIds,
        process.name as processName,
        process.pid as pid,
        t.parent_id as parentId,
        __max_layout_depth(t.track_count, t.track_ids) as maxDepth
      from _process_track_summary_by_upid_and_parent_id_and_name t
      join process using (upid)
      where t.name is null or t.name not glob "* Timeline"
    `);

    const it = result.iter({
      upid: NUM,
      parentId: NUM_NULL,
      trackName: STR_NULL,
      trackIds: STR,
      processName: STR_NULL,
      pid: NUM_NULL,
      maxDepth: NUM,
    });

    const trackMap = new Map<
      number,
      {parentId: number | null; upid: number; trackNode: TrackNode}
    >();

    for (; it.valid(); it.next()) {
      const upid = it.upid;
      const trackName = it.trackName;
      const rawTrackIds = it.trackIds;
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const processName = it.processName;
      const pid = it.pid;
      const maxDepth = it.maxDepth;

      const kind = SLICE_TRACK_KIND;
      const title = getTrackName({
        name: trackName,
        upid,
        pid,
        processName,
        kind,
      });

      const uri = `/process_${upid}/async_slices_${rawTrackIds}`;
      ctx.tracks.registerTrack({
        uri,
        title,
        tags: {
          trackIds,
          kind: SLICE_TRACK_KIND,
          scope: 'process',
          upid,
        },
        track: new TraceProcessorSliceTrack(ctx, uri, maxDepth, trackIds),
      });
      const track = new TrackNode({uri, title, sortOrder: 30});
      trackIds.forEach((id) => {
        trackMap.set(id, {trackNode: track, parentId: it.parentId, upid});
        trackIdsToUris.set(id, uri);
      });
    }

    // Attach track nodes to parents / or the workspace if they have no parent
    trackMap.forEach((t) => {
      const parent = exists(t.parentId) && trackMap.get(t.parentId);
      if (parent !== false && parent !== undefined) {
        parent.trackNode.addChildInOrder(t.trackNode);
      } else {
        const processGroup = ctx.plugins
          .getPlugin(ProcessThreadGroupsPlugin)
          .getGroupForProcess(t.upid);
        processGroup?.addChildInOrder(t.trackNode);
      }
    });
  }

  async addThreadAsyncSliceTracks(
    ctx: Trace,
    trackIdsToUris: Map<number, string>,
  ): Promise<void> {
    const result = await ctx.engine.query(`
      include perfetto module viz.summary.slices;
      include perfetto module viz.summary.threads;
      include perfetto module viz.threads;

      select
        t.utid,
        t.parent_id as parentId,
        thread.upid,
        t.name as trackName,
        thread.name as threadName,
        thread.tid as tid,
        t.track_ids as trackIds,
        __max_layout_depth(t.track_count, t.track_ids) as maxDepth,
        k.is_main_thread as isMainThread,
        k.is_kernel_thread AS isKernelThread
      from _thread_track_summary_by_utid_and_name t
      join _threads_with_kernel_flag k using(utid)
      join thread using (utid)
    `);

    const it = result.iter({
      utid: NUM,
      parentId: NUM_NULL,
      upid: NUM_NULL,
      trackName: STR_NULL,
      trackIds: STR,
      maxDepth: NUM,
      isMainThread: NUM_NULL,
      isKernelThread: NUM,
      threadName: STR_NULL,
      tid: NUM_NULL,
    });

    const trackMap = new Map<
      number,
      {parentId: number | null; utid: number; trackNode: TrackNode}
    >();

    for (; it.valid(); it.next()) {
      const {
        utid,
        parentId,
        upid,
        trackName,
        isMainThread,
        isKernelThread,
        maxDepth,
        threadName,
        tid,
      } = it;
      const rawTrackIds = it.trackIds;
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const title = getTrackName({
        name: trackName,
        utid,
        tid,
        threadName,
        kind: 'Slices',
      });

      const uri = `/${getThreadUriPrefix(upid, utid)}_slice_${rawTrackIds}`;
      ctx.tracks.registerTrack({
        uri,
        title,
        tags: {
          trackIds,
          kind: SLICE_TRACK_KIND,
          scope: 'thread',
          utid,
          upid: upid ?? undefined,
          ...(isKernelThread === 1 && {kernelThread: true}),
        },
        chips: removeFalsyValues([
          isKernelThread === 0 && isMainThread === 1 && 'main thread',
        ]),
        track: new TraceProcessorSliceTrack(ctx, uri, maxDepth, trackIds),
      });
      const track = new TrackNode({uri, title, sortOrder: 20});
      trackIds.forEach((id) => {
        trackMap.set(id, {trackNode: track, parentId, utid});
        trackIdsToUris.set(id, uri);
      });
    }

    // Attach track nodes to parents / or the workspace if they have no parent
    trackMap.forEach((t) => {
      const parent = exists(t.parentId) && trackMap.get(t.parentId);
      if (parent !== false && parent !== undefined) {
        parent.trackNode.addChildInOrder(t.trackNode);
      } else {
        const group = ctx.plugins
          .getPlugin(ProcessThreadGroupsPlugin)
          .getGroupForThread(t.utid);
        group?.addChildInOrder(t.trackNode);
      }
    });
  }
}
