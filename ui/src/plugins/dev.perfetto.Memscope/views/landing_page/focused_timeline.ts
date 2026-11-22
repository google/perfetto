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

import './focused_timeline.scss';
import m from 'mithril';
import {AsyncMemo} from '../../../../base/async_memo';
import {classNames} from '../../../../base/classnames';
import {Time} from '../../../../base/time';
import type {Trace} from '../../../../public/trace';
import {
  COUNTER_TRACK_KIND,
  SLICE_TRACK_KIND,
  SMAPS_TRACK_KIND,
} from '../../../../public/track_kinds';
import {TrackNode} from '../../../../public/workspace';
import {LONG_NULL, NUM, STR} from '../../../../trace_processor/query_result';
import {Button, ButtonVariant} from '../../../../widgets/button';
import {EmptyState} from '../../../../widgets/empty_state';
import {Icon} from '../../../../widgets/icon';
import {Spinner} from '../../../../widgets/spinner';
import TimelinePlugin, {
  type EmbeddedTimelineWindow,
} from '../../../../core_plugins/dev.perfetto.Timeline';

type GroupId = 'captures' | 'counters' | 'runtime' | 'system';

interface DiscoveredTrack {
  readonly uri: string;
  readonly name: string;
  readonly chips?: ReadonlyArray<string>;
}

interface FocusedTimelineData {
  readonly captures: ReadonlyArray<DiscoveredTrack>;
  readonly counters: ReadonlyArray<DiscoveredTrack>;
  readonly runtime: ReadonlyArray<DiscoveredTrack>;
  readonly system: ReadonlyArray<DiscoveredTrack>;
  readonly initialWindow?: EmbeddedTimelineWindow;
}

const PROCESS_COUNTER_PRIORITY: ReadonlyArray<string> = [
  'mem.rss',
  'mem.rss.anon',
  'mem.swap',
  'mem.rss.file',
  'mem.rss.shmem',
  'mem.dmabuf_rss',
  'mem.rss.watermark',
  'oom_score_adj',
];

const SYSTEM_COUNTER_RE =
  /^(mem\.|lmk|MemAvailable|MemFree|Swap|DMA-BUF|dmabuf|ion|psi\.mem|gpu\.mem)/i;

const GC_THREAD_RE =
  /(HeapTaskDaemon|ReferenceQueueDaemon|FinalizerDaemon|RenderThread|binder)/i;

function isCaptureTrack(kinds: ReadonlyArray<string>): boolean {
  return kinds.some(
    (k) =>
      k.startsWith('heap_profile:') ||
      k === 'java_heap_graph' ||
      k === SMAPS_TRACK_KIND ||
      k === 'smaps',
  );
}

function isProcessMemoryCounter(
  name: string,
  kinds: ReadonlyArray<string>,
): boolean {
  if (!kinds.includes(COUNTER_TRACK_KIND)) return false;
  return (
    name.startsWith('mem.') ||
    name === 'oom_score_adj' ||
    /rss|swap|dmabuf|ion|gpu|heap/i.test(name)
  );
}

async function discoverProcessTracks(
  trace: Trace,
  upid: number,
): Promise<FocusedTimelineData> {
  const flat = trace.defaultWorkspace.flatTracks;
  const trackOf = (n: TrackNode) =>
    n.uri === undefined ? undefined : trace.tracks.getTrack(n.uri);

  // Query threads of this process + active slice track ids + memory time span.
  const [utidRes, spanRes] = await Promise.all([
    trace.engine.query(`
      SELECT
        t.utid,
        coalesce(t.name, '') AS name,
        (t.tid = p.pid) AS is_main,
        EXISTS(
          SELECT 1
          FROM thread_track tt
          JOIN slice s ON s.track_id = tt.id
          WHERE tt.utid = t.utid
          LIMIT 1
        ) AS has_slices
      FROM thread t
      JOIN process p ON p.upid = t.upid
      WHERE t.upid = ${upid}
    `),
    trace.engine.query(`
      WITH ts_all AS (
        SELECT ts FROM profiler_smaps WHERE upid = ${upid}
        UNION ALL
        SELECT ts FROM heap_profile_allocation WHERE upid = ${upid}
        UNION ALL
        SELECT graph_sample_ts AS ts FROM heap_graph_object WHERE upid = ${upid}
        UNION ALL
        SELECT c.ts
        FROM counter c
        JOIN process_counter_track t ON t.id = c.track_id
        WHERE t.upid = ${upid} AND t.name IN ('mem.rss', 'mem.rss.anon', 'mem.swap')
      )
      SELECT MIN(ts) AS min_ts, MAX(ts) AS max_ts FROM ts_all
    `),
  ]);

  const interestingUtids = new Map<number, {name: string; role?: string}>();
  for (
    const it = utidRes.iter({
      utid: NUM,
      name: STR,
      is_main: NUM,
      has_slices: NUM,
    });
    it.valid();
    it.next()
  ) {
    if (it.has_slices === 0) continue;
    const isMain = it.is_main !== 0;
    if (isMain || GC_THREAD_RE.test(it.name)) {
      interestingUtids.set(it.utid, {
        name: it.name,
        role: isMain
          ? 'main'
          : /HeapTask|ReferenceQueue|Finalizer/i.test(it.name)
            ? 'GC'
            : undefined,
      });
    }
  }

  const captures: DiscoveredTrack[] = [];
  const counters: DiscoveredTrack[] = [];
  const runtime: DiscoveredTrack[] = [];
  const system: DiscoveredTrack[] = [];
  const seenUris = new Set<string>();

  for (const node of flat) {
    if (node.uri === undefined || seenUris.has(node.uri)) continue;
    const track = trackOf(node);
    const kinds = track?.tags?.kinds ?? [];
    const trackUpid = track?.tags?.upid;
    const trackUtid = track?.tags?.utid;

    if (trackUpid === upid) {
      if (isCaptureTrack(kinds)) {
        seenUris.add(node.uri);
        const chip = kinds.some((k) => k === 'java_heap_graph')
          ? 'Java'
          : kinds.some((k) => k.startsWith('heap_profile:'))
            ? 'Native'
            : 'Smaps';
        captures.push({uri: node.uri, name: node.name, chips: [chip]});
        continue;
      }
      if (isProcessMemoryCounter(node.name, kinds)) {
        seenUris.add(node.uri);
        counters.push({uri: node.uri, name: node.name});
        continue;
      }
      if (
        kinds.includes(SLICE_TRACK_KIND) &&
        /dalvik|gc|art|heap|mem/i.test(node.name)
      ) {
        seenUris.add(node.uri);
        runtime.push({uri: node.uri, name: node.name});
        continue;
      }
    }

    if (
      trackUtid !== undefined &&
      interestingUtids.has(trackUtid) &&
      kinds.includes(SLICE_TRACK_KIND)
    ) {
      seenUris.add(node.uri);
      const meta = interestingUtids.get(trackUtid)!;
      runtime.push({
        uri: node.uri,
        name: node.name || meta.name,
        chips: meta.role ? [meta.role] : undefined,
      });
      continue;
    }

    if (
      trackUpid === undefined &&
      trackUtid === undefined &&
      (kinds.includes(COUNTER_TRACK_KIND) ||
        kinds.includes(SLICE_TRACK_KIND)) &&
      SYSTEM_COUNTER_RE.test(node.name)
    ) {
      seenUris.add(node.uri);
      system.push({uri: node.uri, name: node.name});
    }
  }

  counters.sort((a, b) => {
    const ia = PROCESS_COUNTER_PRIORITY.indexOf(a.name);
    const ib = PROCESS_COUNTER_PRIORITY.indexOf(b.name);
    if (ia !== -1 && ib !== -1) return ia - ib;
    if (ia !== -1) return -1;
    if (ib !== -1) return 1;
    return a.name.localeCompare(b.name);
  });

  const spanRow = spanRes.firstRow({min_ts: LONG_NULL, max_ts: LONG_NULL});
  const initialWindow =
    spanRow.min_ts !== null && spanRow.max_ts !== null
      ? {start: Time.fromRaw(spanRow.min_ts), end: Time.fromRaw(spanRow.max_ts)}
      : undefined;

  return {
    captures,
    counters,
    runtime: runtime.slice(0, 12),
    system: system.slice(0, 10),
    initialWindow,
  };
}

function buildRootNode(
  data: FocusedTimelineData,
  enabled: ReadonlySet<GroupId>,
): TrackNode {
  const root = new TrackNode({name: 'memscope-focused'});

  const addSection = (
    id: GroupId,
    title: string,
    tracks: ReadonlyArray<DiscoveredTrack>,
    chip?: string,
  ) => {
    if (!enabled.has(id) || tracks.length === 0) return;
    const heading = new TrackNode({
      name: title,
      heading: true,
      collapsed: false,
      chips: chip ? [chip] : undefined,
    });
    for (const t of tracks) {
      heading.addChildLast(
        new TrackNode({
          uri: t.uri,
          name: t.name,
          chips: t.chips,
        }),
      );
    }
    root.addChildLast(heading);
  };

  addSection(
    'captures',
    'Memory Captures',
    data.captures,
    `${data.captures.length}`,
  );
  addSection(
    'counters',
    'Process Memory Counters',
    data.counters,
    `${data.counters.length}`,
  );
  addSection(
    'runtime',
    'GC & Key Threads',
    data.runtime,
    `${data.runtime.length}`,
  );
  addSection(
    'system',
    'System Memory Context',
    data.system,
    `${data.system.length}`,
  );

  return root;
}

export interface FocusedTimelineViewAttrs {
  readonly trace: Trace;
  readonly upid: number;
  readonly processName: string;
}

export class FocusedTimelineView implements m.ClassComponent<FocusedTimelineViewAttrs> {
  private readonly memo = new AsyncMemo<FocusedTimelineData>();
  private enabled = new Set<GroupId>(['captures', 'counters', 'runtime']);
  private systemAutoSetForUpid?: number;
  private resetCounter = 0;

  onremove() {
    this.memo.dispose();
  }

  view({attrs}: m.CVnode<FocusedTimelineViewAttrs>): m.Children {
    const {trace, upid, processName} = attrs;
    const res = this.memo.use({
      key: {traceId: trace.traceInfo.uuid, upid},
      compute: () => discoverProcessTracks(trace, upid),
    });

    if (res.data === undefined) {
      return m('.pf-memscope-ft-loading', [
        m(Spinner, {easing: true}),
        m('span', `Building focused timeline for ${processName}…`),
      ]);
    }

    const data = res.data;
    // If there are no process counters, default System Context on so the user
    // still sees memory curves alongside captures.
    if (this.systemAutoSetForUpid !== upid) {
      this.systemAutoSetForUpid = upid;
      if (data.counters.length === 0 && data.system.length > 0) {
        this.enabled.add('system');
      }
    }

    const groups: ReadonlyArray<{
      readonly id: GroupId;
      readonly label: string;
      readonly count: number;
      readonly icon: string;
    }> = [
      {
        id: 'captures',
        label: 'Captures',
        count: data.captures.length,
        icon: 'memory',
      },
      {
        id: 'counters',
        label: 'Process Counters',
        count: data.counters.length,
        icon: 'show_chart',
      },
      {
        id: 'runtime',
        label: 'GC & Threads',
        count: data.runtime.length,
        icon: 'recycling',
      },
      {
        id: 'system',
        label: 'System Context',
        count: data.system.length,
        icon: 'devices',
      },
    ];

    const rootNode = buildRootNode(data, this.enabled);
    const totalVisible = rootNode.children.reduce(
      (sum, h) => sum + h.children.length,
      0,
    );

    const timelinePlugin = trace.plugins.getPlugin(TimelinePlugin);

    return m('.pf-memscope-ft', [
      m('.pf-memscope-ft__bar', [
        m('.pf-memscope-ft__chips', [
          m('span.pf-memscope-ft__label', 'Tracks:'),
          groups.map((g) => {
            const active = this.enabled.has(g.id);
            const empty = g.count === 0;
            return m(
              'button.pf-memscope-ft__chip',
              {
                disabled: empty,
                className: classNames(
                  active && !empty && 'pf-memscope-ft__chip--on',
                  empty && 'pf-memscope-ft__chip--empty',
                ),
                onclick: () => {
                  if (active) this.enabled.delete(g.id);
                  else this.enabled.add(g.id);
                },
              },
              [
                m(Icon, {icon: active && !empty ? 'check' : g.icon}),
                m('span', g.label),
                m('span.pf-memscope-ft__count', `${g.count}`),
              ],
            );
          }),
        ]),
        m('.pf-memscope-ft__actions', [
          m(Button, {
            label: 'Reset zoom',
            icon: 'center_focus_strong',
            variant: ButtonVariant.Minimal,
            compact: true,
            onclick: () => {
              this.resetCounter += 1;
            },
          }),
          m(Button, {
            label: 'Full timeline',
            rightIcon: 'open_in_new',
            variant: ButtonVariant.Outlined,
            compact: true,
            onclick: () => trace.navigate('#!/viewer'),
          }),
        ]),
      ]),
      totalVisible === 0
        ? m(EmptyState, {
            icon: 'view_timeline',
            title: 'No matching tracks selected',
            detail:
              'Enable a track group above or open the full timeline to browse all tracks in this trace.',
          })
        : timelinePlugin.renderEmbeddedTimeline({
            rootNode,
            initialWindow: data.initialWindow,
            windowKey: `${trace.traceInfo.uuid}:${upid}:${this.resetCounter}`,
            showSelectionDetails: true,
            className: 'pf-memscope-ft__canvas',
          }),
    ]);
  }
}
