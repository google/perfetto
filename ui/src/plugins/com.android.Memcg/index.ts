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

import {CounterTrack} from '../../components/tracks/counter_track';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {COUNTER_TRACK_KIND} from '../../public/track_kinds';
import {TrackNode} from '../../public/workspace';
import {NUM, NUM_NULL, STR, STR_NULL} from '../../trace_processor/query_result';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';
import TraceProcessorTrackPlugin from '../dev.perfetto.TraceProcessorTrack';
import {TraceProcessorCounterTrack} from '../dev.perfetto.TraceProcessorTrack/trace_processor_counter_track';

// Same sort order as the generic process-scoped counter tracks (e.g.
// mem.rss.*), so that memcg tracks sit right after them.
const PROCESS_TRACK_SORT_ORDER = 30;

// A memcg track: a raw track of android_memcg_track (trackId set), or a
// series of android_memcg_derived_intervals for one cgroup (trackId null).
interface MemcgTrack {
  readonly trackId: number | null;
  readonly cgroupKey: number;
  readonly level: string; // 'process' | 'uid' | 'group'
  readonly kind: string; // 'size' | 'limit' | 'event' | 'derived'
  readonly memcgGroup: string; // 'apps' | 'system'
  readonly uid: number | null;
  readonly upid: number | null;
  readonly pid: number | null;
  readonly processName: string | null;
  readonly key: string;
  readonly name: string | null;
  readonly packageName: string | null;
  // The number of cgroups of the process, for process-level cgroups.
  readonly processCgroups: number;
}

// Shows the memory cgroup (memcg) counters of Android's cgroup v2 hierarchy
// (apps|system -> uid_<uid> -> pid_<pid>), as recorded by the linux.cgroup
// data source and exposed by the android.memory.memcg stdlib module:
// - process-level tracks go into the process' track group (in a node per
//   cgroup if the process has several, e.g. when a pid was reused under
//   another uid);
// - uid- and top-level tracks go into Memory > Memcg > apps|system, with one
//   uid_<uid> node per uid, which also has a pid_<pid> node per process
//   cgroup of the uid, with copies of the process-level tracks.
// Each cgroup's derived tracks (e.g. headroom) sit next to its raw tracks.
export default class implements PerfettoPlugin {
  static readonly id = 'com.android.Memcg';
  static readonly dependencies = [
    ProcessThreadGroupsPlugin,
    StandardGroupsPlugin,
    TraceProcessorTrackPlugin,
  ];

  private memcgGroup?: TrackNode;
  // apps|system nodes, keyed by memcg_group.
  private readonly topLevelGroups = new Map<string, TrackNode>();
  // uid_<uid> nodes, keyed by memcg_group and uid.
  private readonly uidGroups = new Map<string, TrackNode>();
  // pid_<pid> nodes in the uid nodes, keyed by cgroup_key.
  private readonly pidGroups = new Map<number, TrackNode>();
  // Nodes of the cgroups of processes with several cgroups, in the process'
  // track group, keyed by cgroup_key.
  private readonly processCgroupGroups = new Map<number, TrackNode>();

  async onTraceLoad(ctx: Trace): Promise<void> {
    // One row per raw track and per (cgroup, key) of derived intervals
    // (trackId NULL), so that both kinds can be sorted by key together.
    const result = await ctx.engine.query(`
      include perfetto module android.memory.memcg;

      with cgroup_package as (
        select cgroup_key, min(package_name) as package_name
        from android_memcg_track
        group by cgroup_key
      ),
      derived as (
        select distinct cgroup_key, level, memcg_group, uid, upid, key
        from android_memcg_derived_intervals
        -- No headroom track for a cgroup whose high is always "max".
        where key != 'headroom' or value is not null
      ),
      process_cgroups as (
        select upid, count(distinct cgroup_key) as processCgroups
        from android_memcg_track
        where level = 'process'
        group by upid
      ),
      tracks as (
        select
          id as trackId,
          cgroup_key as cgroupKey,
          level,
          kind,
          memcg_group as memcgGroup,
          uid,
          upid,
          key,
          name,
          package_name as packageName
        from android_memcg_track
        union all
        select
          null as trackId,
          d.cgroup_key as cgroupKey,
          d.level,
          'derived' as kind,
          d.memcg_group as memcgGroup,
          d.uid,
          d.upid,
          d.key,
          null as name,
          p.package_name as packageName
        from derived d
        left join cgroup_package p using (cgroup_key)
      )
      select
        t.*,
        p.pid,
        p.name as processName,
        ifnull(pc.processCgroups, 0) as processCgroups
      from tracks t
      left join process p using (upid)
      left join process_cgroups pc using (upid)
      order by memcgGroup, uid, pid, cgroupKey, key
    `);

    const it = result.iter({
      trackId: NUM_NULL,
      cgroupKey: NUM,
      level: STR,
      kind: STR,
      memcgGroup: STR,
      uid: NUM_NULL,
      upid: NUM_NULL,
      pid: NUM_NULL,
      processName: STR_NULL,
      key: STR,
      name: STR_NULL,
      packageName: STR_NULL,
      processCgroups: NUM,
    });
    for (; it.valid(); it.next()) {
      const track: MemcgTrack = {
        trackId: it.trackId,
        cgroupKey: it.cgroupKey,
        level: it.level,
        kind: it.kind,
        memcgGroup: it.memcgGroup,
        uid: it.uid,
        upid: it.upid,
        pid: it.pid,
        processName: it.processName,
        key: it.key,
        name: it.name,
        packageName: it.packageName,
        processCgroups: it.processCgroups,
      };
      if (track.level !== 'process') {
        this.addTrack(ctx, track, this.getCgroupGroup(ctx, track), '/memcg', 0);
        continue;
      }
      // A process cgroup's tracks go both into the process' group (if it has
      // one) and into the uid tree, as distinct tracks: one node per URI.
      if (track.upid !== null) {
        const processGroup = ctx.plugins
          .getPlugin(ProcessThreadGroupsPlugin)
          .getGroupForProcess(track.upid);
        if (processGroup !== undefined) {
          this.addTrack(
            ctx,
            track,
            track.processCgroups > 1
              ? this.getProcessCgroupGroup(processGroup, track)
              : processGroup,
            '/memcg',
            PROCESS_TRACK_SORT_ORDER,
          );
        }
      }
      if (track.uid !== null) {
        this.addTrack(
          ctx,
          track,
          this.getPidGroup(ctx, track, track.uid),
          '/memcg_uidtree',
          0,
        );
      }
    }
  }

  // Registers a track for |track| with a URI starting with |uriPrefix|, and
  // adds it to |parent|.
  private addTrack(
    ctx: Trace,
    track: MemcgTrack,
    parent: TrackNode,
    uriPrefix: string,
    sortOrder: number,
  ) {
    const {trackId, cgroupKey, kind, key, upid} = track;
    let uri: string;
    let name: string;
    if (trackId !== null) {
      uri = `${uriPrefix}_${trackId}`;
      name = track.name ?? key;
      ctx.tracks.registerTrack({
        uri,
        tags: {
          kinds: [COUNTER_TRACK_KIND],
          trackIds: [trackId],
          upid: upid ?? undefined,
        },
        renderer: new TraceProcessorCounterTrack({
          trace: ctx,
          uri,
          trackId,
          trackName: name,
          unit: kind === 'event' ? undefined : 'B',
          yMode: kind === 'event' ? 'delta' : 'value',
        }),
      });
    } else {
      uri = `${uriPrefix}_derived_${cgroupKey}_${key}`;
      name = derivedTrackName(track);
      // Headroom is NULL while the cgroup is unlimited. Render it as -1,
      // the same "no limit" convention as the raw limit tracks: dropping the
      // rows instead would hold the last finite headroom. -1 can only be
      // the sentinel: memory.current and memory.high are page counters
      // printed in bytes, so a real headroom is a multiple of the page size.
      ctx.tracks.registerTrack({
        uri,
        renderer: CounterTrack.create({
          trace: ctx,
          uri,
          sqlSource: `
            select ts, ifnull(value, -1) as value
            from android_memcg_derived_intervals
            where cgroup_key = ${cgroupKey} and key = '${key}'
          `,
          unit: 'B',
        }),
      });
    }
    parent.addChildInOrder(new TrackNode({uri, name, sortOrder}));
  }

  // The node of a uid-level or top-level cgroup.
  private getCgroupGroup(ctx: Trace, track: MemcgTrack): TrackNode {
    if (track.level === 'uid' && track.uid !== null) {
      return this.getUidGroup(ctx, track.memcgGroup, track.uid, track);
    }
    return this.getTopLevelGroup(ctx, track.memcgGroup);
  }

  // The node of a process' cgroup in the process' track group, for processes
  // with several cgroups.
  private getProcessCgroupGroup(
    processGroup: TrackNode,
    track: MemcgTrack,
  ): TrackNode {
    const {cgroupKey, memcgGroup, uid} = track;
    let group = this.processCgroupGroups.get(cgroupKey);
    if (group === undefined) {
      group = new TrackNode({
        name: `memcg: ${memcgGroup}/uid_${uid}`,
        isSummary: true,
        sortOrder: PROCESS_TRACK_SORT_ORDER,
      });
      processGroup.addChildInOrder(group);
      this.processCgroupGroups.set(cgroupKey, group);
    }
    return group;
  }

  // The pid_<pid> node of a process cgroup, in the node of its uid.
  private getPidGroup(ctx: Trace, track: MemcgTrack, uid: number): TrackNode {
    const {cgroupKey, memcgGroup, pid, upid, processName} = track;
    let group = this.pidGroups.get(cgroupKey);
    if (group === undefined) {
      const id = pid !== null ? `pid_${pid}` : `upid_${upid}`;
      const suffix = processName === null ? '' : ` (${processName})`;
      group = new TrackNode({name: `${id}${suffix}`, isSummary: true});
      // Created even if the uid has no tracks of its own (e.g. when only the
      // cgroups of processes are recorded).
      this.getUidGroup(ctx, memcgGroup, uid, track).addChildInOrder(group);
      this.pidGroups.set(cgroupKey, group);
    }
    return group;
  }

  private getUidGroup(
    ctx: Trace,
    memcgGroup: string,
    uid: number,
    track: MemcgTrack,
  ): TrackNode {
    const mapKey = `${memcgGroup}/${uid}`;
    let group = this.uidGroups.get(mapKey);
    if (group === undefined) {
      const {packageName} = track;
      const suffix = packageName === null ? '' : ` (${packageName})`;
      group = new TrackNode({name: `uid_${uid}${suffix}`, isSummary: true});
      this.getTopLevelGroup(ctx, memcgGroup).addChildInOrder(group);
      this.uidGroups.set(mapKey, group);
    }
    return group;
  }

  private getTopLevelGroup(ctx: Trace, memcgGroup: string): TrackNode {
    let group = this.topLevelGroups.get(memcgGroup);
    if (group === undefined) {
      group = new TrackNode({name: memcgGroup, isSummary: true});
      this.getMemcgGroup(ctx).addChildInOrder(group);
      this.topLevelGroups.set(memcgGroup, group);
    }
    return group;
  }

  private getMemcgGroup(ctx: Trace): TrackNode {
    if (this.memcgGroup === undefined) {
      this.memcgGroup = new TrackNode({name: 'Memcg', isSummary: true});
      ctx.plugins
        .getPlugin(StandardGroupsPlugin)
        .getOrCreateStandardGroup(ctx.defaultWorkspace, 'MEMORY')
        .addChildInOrder(this.memcgGroup);
    }
    return this.memcgGroup;
  }
}

// Mirrors the names trace processor gives to the raw tracks of the same cgroup
// (memcg.<key>, memcg.<group>.uid_<uid>.<key>, memcg.<group>.<key>).
function derivedTrackName(track: MemcgTrack): string {
  const {level, memcgGroup, uid, key} = track;
  switch (level) {
    case 'process':
      return `memcg.${key}`;
    case 'uid':
      return `memcg.${memcgGroup}.uid_${uid}.${key}`;
    default:
      return `memcg.${memcgGroup}.${key}`;
  }
}
