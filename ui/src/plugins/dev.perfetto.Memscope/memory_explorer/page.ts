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
import {AsyncMemo} from '../../../base/async_memo';
import {formatBytesIec} from '../../../base/bytes_format';
import {classNames} from '../../../base/classnames';
import {Gate} from '../../../base/mithril_utils';
import type {time} from '../../../base/time';
import type {Setting} from '../../../public/settings';
import type {Trace} from '../../../public/trace';
import {LONG, NUM, STR} from '../../../trace_processor/query_result';
import {Button, ButtonGroup, ButtonVariant} from '../../../widgets/button';
import {Intent} from '../../../widgets/common';
import {EmptyState} from '../../../widgets/empty_state';
import {Icon} from '../../../widgets/icon';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {PopupPosition} from '../../../widgets/popup';
import {Spinner} from '../../../widgets/spinner';
import {Tabs, type TabsTab} from '../../../widgets/tabs';
import {
  buildHeapDumpTabs,
  heapDumpActiveTabKey,
} from '../../com.android.HeapDumpExplorer/heap_dump_views';
import {
  isNativeView,
  subpageToMemoryLocation,
  type NavState,
  type NavView,
} from '../../com.android.HeapDumpExplorer/nav_state';
import * as hdeQueries from '../../com.android.HeapDumpExplorer/queries';
import type {HeapDumpExplorerSession} from '../../com.android.HeapDumpExplorer/session';
import type {HeapDump} from '../../com.android.HeapDumpExplorer/types';
import {Callout} from '../components/callout';
import {Page, SubPage} from '../components/page';
import {ProcessHeader} from '../components/process_header';
import {matchProcessesByName} from '../process_focus';
import {
  FileBackedDomainView,
  GraphicsDomainView,
  OtherMemDomainView,
  SmapsFallbackDomainView,
  StacksDomainView,
} from '../views/domains/domain_views';
import {
  MemoryAreasPanel,
  type MemoryAreaCardModel,
  type MemoryAreaId,
} from '../views/domains/memory_areas_panel';
import {FocusedTimelineView} from '../views/landing_page/focused_timeline';
import {ProcessMemDetails} from '../views/landing_page/proc_mem_overview';
import {
  type ProcMemStat,
  type ProcWithMem,
  loadProcessMemoryStats,
  pickBestProc,
} from '../views/landing_page/proc_mem_stats';
import type {HeapDumpLink} from '../views/landing_page/section_widgets';
import {SmapsDetail} from '../views/landing_page/smaps_detail';
import {NativeCallsitesView} from '../views/native/native_callsites_view';
import {
  renderDomainEmptyState,
  type MemoryTriageSummary,
} from '../views/native/native_explainer';
import {NativeFlamegraphView} from '../views/native/native_flamegraph_view';
import {NativeGrowthView} from '../views/native/native_growth_view';
import {NativeOverviewView} from '../views/native/native_overview_view';
import {
  ALL_HEAPS,
  loadNativeProfileMeta,
  type NativeProfileMeta,
  type NativeWindow,
} from '../views/native/native_queries';
import {renderExplorerPreparingView} from './loading_page';
import {memoryExplorerHref, rememberMemoryExplorerHref} from './route';
import '../views/landing_page/memory_overview_page.scss';
import '../views/native/native.scss';
import './styles.scss';

export interface MemoryExplorerPageAttrs {
  readonly trace: Trace;
  readonly session: HeapDumpExplorerSession;
  readonly subpage: string | undefined;
  // The route's `process` arg; picks the process when the route has no upid.
  readonly focusProcess?: string;
  readonly autoNavigated: boolean;
  readonly openByDefault: Setting<boolean>;
  readonly hideDefaultChangedHint: Setting<boolean>;
}

type DomainPillKey =
  | 'overview'
  | 'native'
  | 'java'
  | 'file'
  | 'graphics'
  | 'stack'
  | 'other'
  | 'smaps'
  | 'timeline';

interface ProcessTriageBundle extends MemoryTriageSummary {
  readonly javaDumpSizes: ReadonlyMap<bigint, number>;
  readonly nativeRssSwapBytes: number;
  readonly javaRssSwapBytes: number;
  readonly stackSmapsBytes: number;
  readonly otherSmapsBytes: number;
  readonly totalSmapsBytes: number;
  readonly fileDeltaBytes?: number;
  readonly graphicsDeltaBytes?: number;
  readonly stackDeltaBytes?: number;
  readonly otherDeltaBytes?: number;
  readonly gfxCounterBytes: number;
  readonly threadCount: number;
}

const NO_CLASSES: ReadonlySet<string> = new Set();

function isDynamicHdeKey(key: string): boolean {
  return key.startsWith('fg-') || key.startsWith('inst-');
}

function formatRelSec(trace: Trace, ts: time): string {
  const sec = Number(ts - trace.traceInfo.start) / 1e9;
  return `${sec.toFixed(1)}s`;
}

async function loadProcessTriageSummary(
  trace: Trace,
  proc: ProcMemStat,
): Promise<ProcessTriageBundle> {
  const upid = proc.upid;
  let nativeSmapsBytes = 0;
  let javaSmapsBytes = 0;
  let nativeRssSwapBytes = 0;
  let javaRssSwapBytes = 0;
  let graphicsSmapsBytes = 0;
  let fileSmapsBytes = 0;
  let stackSmapsBytes = 0;
  let otherSmapsBytes = 0;
  let totalSmapsBytes = 0;
  let nativeDeltaBytes: number | undefined;
  let javaDeltaBytes: number | undefined;
  let fileDeltaBytes: number | undefined;
  let graphicsDeltaBytes: number | undefined;
  let stackDeltaBytes: number | undefined;
  let otherDeltaBytes: number | undefined;

  if (proc.smapsSnapshots > 0) {
    const res = await trace.engine.query(`
      INCLUDE PERFETTO MODULE memscope.smaps;
      SELECT ts, category, rss_swap_bytes AS rss_swap, anon_swap_bytes AS anon_swap
      FROM memscope_smaps_category_breakdown
      WHERE upid = ${upid}
      ORDER BY ts ASC
    `);
    const snaps = new Map<
      bigint,
      Map<string, {rssSwap: number; anonSwap: number}>
    >();
    const tsOrder: bigint[] = [];
    for (
      const it = res.iter({
        ts: LONG,
        category: STR,
        rss_swap: NUM,
        anon_swap: NUM,
      });
      it.valid();
      it.next()
    ) {
      let byCat = snaps.get(it.ts);
      if (byCat === undefined) {
        byCat = new Map();
        snaps.set(it.ts, byCat);
        tsOrder.push(it.ts);
      }
      byCat.set(it.category, {rssSwap: it.rss_swap, anonSwap: it.anon_swap});
    }
    if (tsOrder.length > 0) {
      const last = snaps.get(tsOrder[tsOrder.length - 1]!)!;
      nativeSmapsBytes = last.get('native')?.anonSwap ?? 0;
      javaSmapsBytes = last.get('java')?.anonSwap ?? 0;
      nativeRssSwapBytes = last.get('native')?.rssSwap ?? 0;
      javaRssSwapBytes = last.get('java')?.rssSwap ?? 0;
      graphicsSmapsBytes = last.get('graphics')?.rssSwap ?? 0;
      fileSmapsBytes = last.get('file')?.rssSwap ?? 0;
      stackSmapsBytes = last.get('stack')?.rssSwap ?? 0;
      otherSmapsBytes = last.get('other')?.rssSwap ?? 0;
      totalSmapsBytes =
        nativeRssSwapBytes +
        javaRssSwapBytes +
        graphicsSmapsBytes +
        fileSmapsBytes +
        stackSmapsBytes +
        otherSmapsBytes;
      if (tsOrder.length >= 2) {
        const first = snaps.get(tsOrder[0]!)!;
        nativeDeltaBytes =
          nativeRssSwapBytes - (first.get('native')?.rssSwap ?? 0);
        javaDeltaBytes = javaRssSwapBytes - (first.get('java')?.rssSwap ?? 0);
        fileDeltaBytes = fileSmapsBytes - (first.get('file')?.rssSwap ?? 0);
        graphicsDeltaBytes =
          graphicsSmapsBytes - (first.get('graphics')?.rssSwap ?? 0);
        stackDeltaBytes = stackSmapsBytes - (first.get('stack')?.rssSwap ?? 0);
        otherDeltaBytes = otherSmapsBytes - (first.get('other')?.rssSwap ?? 0);
      }
    }
  }

  let gfxCounterBytes = 0;
  try {
    const res = await trace.engine.query(`
      SELECT CAST(ifnull(MAX(latest), 0) AS INT) AS total
      FROM (
        SELECT t.name, (
          SELECT c.value FROM counter c
          WHERE c.track_id = t.id
          ORDER BY c.ts DESC LIMIT 1
        ) AS latest
        FROM process_counter_track t
        WHERE t.upid = ${upid} AND t.name IN ('mem.dmabuf_rss', 'gpu_memory')
      )
    `);
    gfxCounterBytes = res.firstRow({total: NUM}).total;
  } catch {
    // Ignore if counter tables are unavailable.
  }

  let threadCount = 0;
  try {
    const res = await trace.engine.query(
      `SELECT COUNT(*) AS cnt FROM thread WHERE upid = ${upid}`,
    );
    threadCount = res.firstRow({cnt: NUM}).cnt;
  } catch {
    // Ignore.
  }

  let heapprofdUnreleasedBytes = 0;
  if (proc.nativeDumps > 0) {
    const res = await trace.engine.query(`
      WITH per_site AS (
        SELECT callsite_id, SUM(size) AS net
        FROM heap_profile_allocation
        WHERE upid = ${upid}
        GROUP BY callsite_id
      )
      SELECT CAST(ifnull(SUM(CASE WHEN net > 0 THEN net ELSE 0 END), 0) AS INT) AS total
      FROM per_site
    `);
    heapprofdUnreleasedBytes = res.firstRow({total: NUM}).total;
  }

  let javaHeapBytes = 0;
  let javaRegisteredNativeBytes = 0;
  const javaDumpSizes = new Map<bigint, number>();
  if (proc.heapDumps > 0) {
    try {
      const res = await trace.engine.query(`
        INCLUDE PERFETTO MODULE memscope.java;
        SELECT ts, reachable_heap_size AS heap, reachable_native_size AS nat
        FROM memscope_java_dump
        WHERE upid = ${upid}
        ORDER BY ts ASC
      `);
      for (
        const it = res.iter({ts: LONG, heap: NUM, nat: NUM});
        it.valid();
        it.next()
      ) {
        javaDumpSizes.set(it.ts, it.heap);
        javaHeapBytes = it.heap;
        javaRegisteredNativeBytes = it.nat;
      }
    } catch {
      // Leave zeros if module unavailable.
    }
  }

  return {
    nativeSmapsBytes,
    javaSmapsBytes,
    graphicsSmapsBytes,
    fileSmapsBytes,
    nativeDeltaBytes,
    javaDeltaBytes,
    fileDeltaBytes,
    graphicsDeltaBytes,
    stackDeltaBytes,
    otherDeltaBytes,
    heapprofdUnreleasedBytes,
    javaHeapBytes,
    javaRegisteredNativeBytes,
    hasSmaps: proc.smapsSnapshots > 0,
    hasNativeProfile: proc.nativeDumps > 0,
    hasJavaDump: proc.heapDumps > 0,
    javaDumpSizes,
    nativeRssSwapBytes,
    javaRssSwapBytes,
    stackSmapsBytes,
    otherSmapsBytes,
    totalSmapsBytes,
    gfxCounterBytes,
    threadCount,
  };
}

// Timestamp-and-size-only Java heap dump picker scoped to the active process.
function renderJavaDumpPicker(
  trace: Trace,
  dumps: ReadonlyArray<HeapDump>,
  active: HeapDump,
  dumpSizes: ReadonlyMap<bigint, number> | undefined,
  onSelect: (d: HeapDump) => void,
): m.Children {
  if (dumps.length === 0) return null;

  const formatShort = (d: HeapDump, idx: number): string => {
    const size = dumpSizes?.get(d.ts);
    const parts = [`#${idx + 1}`, formatRelSec(trace, d.ts)];
    if (size !== undefined && size > 0) parts.push(formatBytesIec(size));
    return parts.join(' · ');
  };

  if (dumps.length === 1) {
    return m(
      '.pf-memscope-dump-picker.pf-memscope-dump-picker--single',
      `Dump ${formatShort(dumps[0]!, 0)}`,
    );
  }

  if (dumps.length <= 3) {
    return m('.pf-memscope-dump-picker', [
      m('span.pf-memscope-dump-picker__label', 'Dump'),
      m(
        '.pf-memscope-dump-picker__pills',
        dumps.map((d, idx) =>
          m(
            'button.pf-memscope-dump-picker__pill',
            {
              key: d.ts.toString(),
              type: 'button',
              className: classNames(
                d.ts === active.ts && 'pf-memscope-dump-picker__pill--active',
              ),
              title: `Switch to Java heap dump ${formatShort(d, idx)}`,
              onclick: () => onSelect(d),
            },
            formatShort(d, idx),
          ),
        ),
      ),
    ]);
  }

  const activeIdx = Math.max(
    0,
    dumps.findIndex((d) => d.ts === active.ts),
  );
  return m('.pf-memscope-dump-picker', [
    m('span.pf-memscope-dump-picker__label', 'Dump'),
    m(
      PopupMenu,
      {
        trigger: m(Button, {
          label: formatShort(active, activeIdx),
          rightIcon: 'arrow_drop_down',
          variant: ButtonVariant.Outlined,
          compact: true,
        }),
        position: PopupPosition.BottomEnd,
      },
      dumps.map((d, idx) =>
        m(MenuItem, {
          label: `Dump ${formatShort(d, idx)}`,
          active: d.ts === active.ts,
          onclick: () => onSelect(d),
        }),
      ),
    ),
  ]);
}

interface DomainPillDef {
  readonly key: DomainPillKey;
  readonly label: string;
  readonly icon: string;
  readonly color?: string;
  readonly bytes?: number;
  readonly muted?: boolean;
  readonly dividerBefore?: boolean;
}

// Closed-domain top-level pill bar + inner underline tab strips:
//   Top bar (pills): Overview | Native | Java | File-backed | Graphics | Thread stacks | [Other] | [Smaps] | Timeline
//   Inner bar (underline Tabs):
//     - Java: Overview | Flamegraph | Classes | Objects | Dominators | Bitmaps | Strings | Arrays | Callstack + closable Object / FG tabs + Dump picker
//     - Native: Overview | Flamegraph | Libraries & Callsites | Growth & Churn
// Route: /memory/<upid>/<nav subpage>.
export class MemoryExplorerPage implements m.ClassComponent<MemoryExplorerPageAttrs> {
  private readonly procsMemo = new AsyncMemo<ProcWithMem>();
  private readonly mergedClassesMemo = new AsyncMemo<ReadonlySet<string>>();
  private readonly triageMemo = new AsyncMemo<ProcessTriageBundle>();
  private readonly nativeMetaMemo = new AsyncMemo<NativeProfileMeta>();

  private lastUpid?: number;
  private nativeWindow: NativeWindow = {heapName: ALL_HEAPS};
  private lastNativeView: NavView = 'native-overview';
  private lastJavaNav: {view: NavView; params: Record<string, unknown>} = {
    view: 'overview',
    params: {},
  };
  private activatedDomains = new Set<DomainPillKey>(['overview']);

  onremove() {
    this.procsMemo.dispose();
    this.mergedClassesMemo.dispose();
    this.triageMemo.dispose();
    this.nativeMetaMemo.dispose();
  }

  private resolveDomain(nav: NavState): DomainPillKey {
    const v = nav.view;
    if (v === 'summary') return 'overview';
    if (v === 'smaps') return 'smaps';
    if (v === 'timeline') return 'timeline';
    if (isNativeView(v)) {
      this.lastNativeView = v;
      return 'native';
    }
    if (v === 'file-backed') return 'file';
    if (v === 'graphics') return 'graphics';
    if (v === 'stacks') return 'stack';
    if (v === 'other-mem') return 'other';
    // Any Java view, including dynamic 'object' and 'flamegraph-objects' tabs!
    this.lastJavaNav = {view: v, params: {...nav.params}};
    return 'java';
  }

  private defaultNavForDomain(
    key: DomainPillKey,
    session: HeapDumpExplorerSession,
  ): {view: NavView; params?: Record<string, unknown>} {
    switch (key) {
      case 'overview':
        return {view: 'summary'};
      case 'native':
        return {view: this.lastNativeView};
      case 'java': {
        const last = this.lastJavaNav;
        if (last.view === 'object') {
          const id = Number(last.params.id);
          if (!session.instanceTabs.some((t) => t.objId === id)) {
            return {view: 'overview'};
          }
        } else if (last.view === 'flamegraph-objects') {
          if (session.flamegraphTabs.length === 0) {
            return {view: 'overview'};
          }
        }
        return last;
      }
      case 'file':
        return {view: 'file-backed'};
      case 'graphics':
        return {view: 'graphics'};
      case 'stack':
        return {view: 'stacks'};
      case 'other':
        return {view: 'other-mem'};
      case 'smaps':
        return {view: 'smaps'};
      case 'timeline':
        return {view: 'timeline'};
    }
  }

  view({attrs}: m.Vnode<MemoryExplorerPageAttrs>): m.Children {
    const {trace, session, subpage} = attrs;
    const procs = this.procsMemo.use({
      key: trace.traceInfo.uuid,
      compute: () => loadProcessMemoryStats(trace.engine),
    }).data;
    if (procs === undefined) {
      return renderExplorerPreparingView(
        trace.traceInfo.traceTitle || 'Memory trace',
        attrs.focusProcess,
      );
    }

    const loc = subpageToMemoryLocation(subpage);
    const proc = procs.find((p) => p.upid === loc.upid);
    if (proc === undefined) {
      const named =
        attrs.focusProcess === undefined
          ? procs
          : matchProcessesByName(procs, attrs.focusProcess, (p) => p.procName);
      const best = pickBestProc(named) ?? pickBestProc(procs);
      if (best === undefined) {
        return m(EmptyState, {title: 'No processes with memory in this trace'});
      }
      location.replace(memoryExplorerHref(best.upid));
      return renderExplorerPreparingView(
        trace.traceInfo.traceTitle || 'Memory trace',
        best.procName,
      );
    }

    const upid = proc.upid;
    if (this.lastUpid !== upid) {
      this.lastUpid = upid;
      this.nativeWindow = {heapName: ALL_HEAPS};
      this.lastNativeView = 'native-overview';
      this.lastJavaNav = {view: 'overview', params: {}};
      this.activatedDomains = new Set(['overview']);
    }

    session.selectProcess(upid);
    session.syncFromSubpage(subpage?.replace(/^\/?\d+\/?/, '') || 'summary');
    const dump = session.activeDump;

    const hasJavaCapture = proc.heapDumps > 0;
    const hasSmaps = proc.smapsSnapshots > 0;
    const hasNative = proc.nativeDumps > 0;

    let nav = session.nav;
    if (nav.view === 'smaps' && !hasSmaps) {
      nav = {view: 'summary', params: {}};
    }

    if (dump !== null) {
      session.syncInstanceTabFromNav();
      session.syncFlamegraphTabFromNav();
    }

    const activeDomain = this.resolveDomain(nav);
    this.activatedDomains.add(activeDomain);

    rememberMemoryExplorerHref(
      trace.traceInfo.uuid,
      memoryExplorerHref(upid, nav),
    );

    const triage = this.triageMemo.use({
      key: {traceId: trace.traceInfo.uuid, upid},
      compute: () => loadProcessTriageSummary(trace, proc),
    }).data;

    const nativeMeta = hasNative
      ? this.nativeMetaMemo.use({
          key: {traceId: trace.traceInfo.uuid, upid},
          compute: () => loadNativeProfileMeta(trace, upid),
        }).data
      : undefined;

    const openDomain = (
      key: DomainPillKey,
      targetView?: NavView,
      params?: Record<string, unknown>,
    ) => {
      if (targetView !== undefined) {
        session.navigate(targetView, params);
        return;
      }
      const def = this.defaultNavForDomain(key, session);
      session.navigate(def.view, def.params);
    };

    const openNativeFlamegraph = (filter?: string) => {
      openDomain('native', 'native-flamegraph', filter ? {q: filter} : {});
    };

    const heapDumpLink: HeapDumpLink = (ts, target) => ({
      href: memoryExplorerHref(upid, target),
      onclick: () => {
        const d = session.dumpsForUpid(upid).find((x) => x.ts === ts);
        if (d !== undefined) session.selectDump(d);
      },
    });

    const nativeBytes = hasSmaps
      ? (triage?.nativeRssSwapBytes ?? 0) > 0
        ? triage!.nativeRssSwapBytes
        : (triage?.heapprofdUnreleasedBytes ?? 0)
      : hasNative
        ? (triage?.heapprofdUnreleasedBytes ?? 0)
        : undefined;
    const javaBytes = hasSmaps
      ? (triage?.javaRssSwapBytes ?? 0) > 0
        ? triage!.javaRssSwapBytes
        : (triage?.javaHeapBytes ?? 0)
      : hasJavaCapture
        ? (triage?.javaHeapBytes ?? 0)
        : undefined;
    const fileBytes = hasSmaps ? (triage?.fileSmapsBytes ?? 0) : undefined;
    const gfxBytes = hasSmaps
      ? (triage?.graphicsSmapsBytes ?? 0) > 0
        ? triage!.graphicsSmapsBytes
        : (triage?.gfxCounterBytes ?? 0)
      : (triage?.gfxCounterBytes ?? 0) > 0
        ? triage!.gfxCounterBytes
        : undefined;
    const stackBytes = hasSmaps ? (triage?.stackSmapsBytes ?? 0) : undefined;
    const otherBytes = triage?.otherSmapsBytes ?? 0;

    const pills: DomainPillDef[] = [
      {key: 'overview', label: 'Overview', icon: 'dashboard'},
      {
        key: 'native',
        label: 'Native',
        icon: 'memory',
        color: '#3b82f6',
        bytes: nativeBytes,
        muted: !hasNative && (nativeBytes ?? 0) === 0,
      },
      {
        key: 'java',
        label: 'Java',
        icon: 'local_cafe',
        color: '#d97706',
        bytes: javaBytes,
        muted: !hasJavaCapture && (javaBytes ?? 0) === 0,
      },
      {
        key: 'file',
        label: 'File-backed',
        icon: 'description',
        color: '#2e9e4c',
        bytes: fileBytes,
        muted: (fileBytes ?? 0) === 0,
      },
      {
        key: 'graphics',
        label: 'Graphics',
        icon: 'brush',
        color: '#9333ea',
        bytes: gfxBytes,
        muted: (gfxBytes ?? 0) === 0,
      },
      {
        key: 'stack',
        label: 'Thread stacks',
        icon: 'view_stream',
        color: '#0891b2',
        bytes: stackBytes,
        muted: (stackBytes ?? 0) === 0 && (triage?.threadCount ?? 0) === 0,
      },
      ...(otherBytes > 0
        ? [
            {
              key: 'other' as const,
              label: 'Other',
              icon: 'more_horiz',
              color: '#6b7280',
              bytes: otherBytes,
              muted: false,
            },
          ]
        : []),
      ...(hasSmaps
        ? [
            {
              key: 'smaps' as const,
              label: 'Smaps',
              icon: 'table_rows',
              dividerBefore: true,
            },
          ]
        : []),
      {
        key: 'timeline',
        label: 'Timeline',
        icon: 'view_timeline',
        dividerBefore: !hasSmaps,
      },
    ];

    const renderDomainBody = (key: DomainPillKey): m.Children => {
      switch (key) {
        case 'overview':
          return m(
            Page,
            m(ProcessMemDetails, {
              trace,
              upid,
              tab: 'summary',
              heapDumpLink,
              triage,
              renderAreasPanel: (selTs) =>
                this.buildAreasPanel(
                  trace,
                  proc,
                  triage,
                  hasSmaps,
                  hasNative,
                  hasJavaCapture,
                  selTs,
                  openDomain,
                ),
              onOpenNative: () => openDomain('native'),
              onOpenJava: () => openDomain('java'),
              onOpenSmaps: hasSmaps ? () => openDomain('smaps') : undefined,
              onOpenTimeline: () => openDomain('timeline'),
            }),
          );
        case 'native':
          return this.renderNativeTabBody({
            trace,
            upid,
            proc,
            nav,
            hasNative,
            hasSmaps,
            triage,
            nativeMeta,
            session,
            openNativeFlamegraph,
            openDomain,
          });
        case 'java':
          return this.renderJavaTabBody({
            trace,
            upid,
            proc,
            dump,
            nav,
            hasJavaCapture,
            hasSmaps,
            triage,
            session,
            openDomain,
          });
        case 'file':
          return m(FileBackedDomainView, {
            trace,
            upid,
            processName: proc.procName,
            onOpenSmaps: hasSmaps ? () => openDomain('smaps') : undefined,
            onOpenOverview: () => openDomain('overview'),
          });
        case 'graphics':
          return m(GraphicsDomainView, {
            trace,
            upid,
            processName: proc.procName,
            hasJavaDump: hasJavaCapture,
            onOpenJavaBitmaps: hasJavaCapture
              ? () => openDomain('java', 'bitmaps')
              : undefined,
            onOpenSmaps: hasSmaps ? () => openDomain('smaps') : undefined,
            onOpenOverview: () => openDomain('overview'),
          });
        case 'stack':
          return m(StacksDomainView, {
            trace,
            upid,
            processName: proc.procName,
            onOpenSmaps: hasSmaps ? () => openDomain('smaps') : undefined,
            onOpenOverview: () => openDomain('overview'),
          });
        case 'other':
          return m(OtherMemDomainView, {
            trace,
            upid,
            processName: proc.procName,
            onOpenSmaps: hasSmaps ? () => openDomain('smaps') : undefined,
            onOpenOverview: () => openDomain('overview'),
          });
        case 'smaps':
          return m(Page, m(SubPage, m(SmapsDetail, {trace, upid})));
        case 'timeline':
          return m(FocusedTimelineView, {
            trace,
            upid,
            processName: proc.procName,
          });
      }
    };

    const pillKeys: DomainPillKey[] = pills.map((p) => p.key);

    return m(
      '.pf-hde-page',
      m('.pf-memory-explorer__header', [
        m(ProcessHeader, {
          title: 'Memory Explorer',
          options: procs.map((p) => ({
            key: `${p.upid}`,
            name: p.procName,
            pid: p.pid,
            detail: captureSummary(p),
          })),
          selectedKey: `${upid}`,
          onSelect: (o) => trace.navigate(memoryExplorerHref(Number(o.key))),
        }),
      ]),
      this.renderDefaultChangedHint(attrs),
      m('.pf-memscope-workspace', [
        m(
          '.pf-memscope-domain-bar',
          pills.map((pill) =>
            m(
              'button.pf-memscope-domain-pill',
              {
                key: pill.key,
                type: 'button',
                className: classNames(
                  activeDomain === pill.key &&
                    'pf-memscope-domain-pill--active',
                  pill.muted &&
                    activeDomain !== pill.key &&
                    'pf-memscope-domain-pill--muted',
                  pill.dividerBefore && 'pf-memscope-domain-pill--sep',
                ),
                style: pill.color
                  ? {'--pf-memscope-pill-accent': pill.color}
                  : undefined,
                onclick: () => openDomain(pill.key),
              },
              [
                m(Icon, {
                  icon: pill.icon,
                  style: pill.color ? {color: pill.color} : undefined,
                }),
                m('span.pf-memscope-domain-pill__label', pill.label),
                pill.bytes !== undefined &&
                  pill.bytes > 0 &&
                  m(
                    'span.pf-memscope-domain-pill__size',
                    formatBytesIec(pill.bytes),
                  ),
              ],
            ),
          ),
        ),
        m(
          '.pf-memscope-domain-stage',
          pillKeys.map((key) =>
            m(
              Gate,
              {key: `${upid}:${key}`, open: activeDomain === key},
              this.activatedDomains.has(key)
                ? renderDomainBody(key)
                : undefined,
            ),
          ),
        ),
      ]),
    );
  }

  private renderNativeTabBody(opts: {
    readonly trace: Trace;
    readonly upid: number;
    readonly proc: ProcMemStat;
    readonly nav: NavState;
    readonly hasNative: boolean;
    readonly hasSmaps: boolean;
    readonly triage?: ProcessTriageBundle;
    readonly nativeMeta?: NativeProfileMeta;
    readonly session: HeapDumpExplorerSession;
    readonly openNativeFlamegraph: (filter?: string) => void;
    readonly openDomain: (key: DomainPillKey, view?: NavView) => void;
  }): m.Children {
    const {
      trace,
      upid,
      proc,
      nav,
      hasNative,
      hasSmaps,
      triage,
      nativeMeta,
      session,
      openNativeFlamegraph,
      openDomain,
    } = opts;
    if (!hasNative) {
      return hasSmaps && (triage?.nativeRssSwapBytes ?? 0) > 0
        ? m(SmapsFallbackDomainView, {
            trace,
            upid,
            domain: 'native',
            processName: proc.procName,
            onOpenSmaps: () => openDomain('smaps'),
          })
        : renderDomainEmptyState({
            domain: 'native',
            smapsBytes: triage?.nativeSmapsBytes,
            processName: proc.procName,
            onBackToOverview: () => openDomain('overview'),
          });
    }
    if (nativeMeta === undefined) {
      return m('.pf-hde-loading', m(Spinner, {easing: true}));
    }

    const activeView = isNativeView(nav.view) ? nav.view : this.lastNativeView;
    const win = this.nativeWindow;
    const onWindowChange = (next: NativeWindow) => {
      this.nativeWindow = next;
    };

    const nativeTabs: TabsTab[] = [
      {
        key: 'native-overview',
        title: 'Overview',
        leftIcon: 'dashboard',
        content: m(NativeOverviewView, {
          trace,
          upid,
          meta: nativeMeta,
          window: win,
          onWindowChange,
          onOpenFlamegraph: openNativeFlamegraph,
          onOpenCallsites: () => session.navigate('native-callsites'),
          onOpenGrowth: () => session.navigate('native-growth'),
        }),
      },
      {
        key: 'native-flamegraph',
        title: 'Flamegraph',
        leftIcon: 'local_fire_department',
        lazy: true,
        content: m(NativeFlamegraphView, {
          trace,
          upid,
          meta: nativeMeta,
          window: win,
          onWindowChange,
          initialFilter:
            nav.view === 'native-flamegraph' ? nav.params.q : undefined,
          onConsumedFilter: () => session.clearNavParam('q'),
        }),
      },
      {
        key: 'native-callsites',
        title: 'Libraries & Callsites',
        leftIcon: 'library_books',
        lazy: true,
        content: m(NativeCallsitesView, {
          trace,
          upid,
          meta: nativeMeta,
          window: win,
          onWindowChange,
          onOpenFlamegraph: openNativeFlamegraph,
        }),
      },
      {
        key: 'native-growth',
        title: 'Growth & Churn',
        leftIcon: 'trending_up',
        lazy: true,
        content: m(NativeGrowthView, {
          trace,
          upid,
          meta: nativeMeta,
          window: win,
          onWindowChange,
          onOpenFlamegraph: openNativeFlamegraph,
        }),
      },
    ];

    return m(Tabs, {
      key: `native-tabs:${upid}`,
      className: 'pf-memscope-inner-tabs',
      variant: 'underline',
      tabs: nativeTabs,
      activeTabKey: activeView,
      onTabChange: (key) => session.navigate(key as NavView),
    });
  }

  private renderJavaTabBody(opts: {
    readonly trace: Trace;
    readonly upid: number;
    readonly proc: ProcMemStat;
    readonly dump: HeapDump | null;
    readonly nav: NavState;
    readonly hasJavaCapture: boolean;
    readonly hasSmaps: boolean;
    readonly triage?: ProcessTriageBundle;
    readonly session: HeapDumpExplorerSession;
    readonly openDomain: (key: DomainPillKey, view?: NavView) => void;
  }): m.Children {
    const {
      trace,
      upid,
      proc,
      dump,
      nav,
      hasJavaCapture,
      hasSmaps,
      triage,
      session,
      openDomain,
    } = opts;
    if (dump === null) {
      if (hasJavaCapture) {
        return m('.pf-hde-loading', m(Spinner, {easing: true}));
      }
      return hasSmaps && (triage?.javaRssSwapBytes ?? 0) > 0
        ? m(SmapsFallbackDomainView, {
            trace,
            upid,
            domain: 'java',
            processName: proc.procName,
            onOpenSmaps: () => openDomain('smaps'),
          })
        : renderDomainEmptyState({
            domain: 'java',
            smapsBytes: triage?.javaSmapsBytes,
            processName: proc.procName,
            onBackToOverview: () => openDomain('overview'),
          });
    }

    const merged = this.mergedClassesMemo.use({
      key: {upid: dump.upid, ts: dump.ts},
      compute: () =>
        hdeQueries.getPotentiallyMergedClasses(session.engine, dump),
    }).data;
    const hde = buildHeapDumpTabs(
      session,
      dump,
      nav,
      merged ?? NO_CLASSES,
      'Overview',
    );

    // Style dynamic closable object / FG tabs with a subtle divider & icon
    // right here inside the Java tab strip (never at the top domain bar!).
    let firstDyn = true;
    const javaTabs: TabsTab[] = hde.tabs.map((t) => {
      if (!isDynamicHdeKey(t.key)) return t;
      const isFirst = firstDyn;
      firstDyn = false;
      return {
        ...t,
        title: m(
          'span.pf-memscope-dyn-tab',
          {className: classNames(isFirst && 'pf-memscope-dyn-tab--first')},
          t.title,
        ),
        leftIcon: m(Icon, {
          icon: t.key.startsWith('fg-') ? 'filter_center_focus' : 'data_object',
          style: {color: '#d97706'},
        }),
      };
    });

    const inJava =
      nav.view !== 'summary' &&
      nav.view !== 'smaps' &&
      nav.view !== 'timeline' &&
      nav.view !== 'file-backed' &&
      nav.view !== 'graphics' &&
      nav.view !== 'stacks' &&
      nav.view !== 'other-mem' &&
      !isNativeView(nav.view);
    const activeTabKey = inJava
      ? heapDumpActiveTabKey(session)
      : this.lastJavaNav.view;

    return m(Tabs, {
      key: `java-tabs:${upid}:${dump.ts}`,
      className: 'pf-memscope-inner-tabs',
      variant: 'underline',
      tabs: javaTabs,
      activeTabKey,
      onTabChange: (key) => hde.actions.get(key)?.select(),
      onTabClose: (key) => hde.actions.get(key)?.close?.(),
      rightContent: renderJavaDumpPicker(
        trace,
        session.dumpsForUpid(upid),
        dump,
        triage?.javaDumpSizes,
        (d) => session.selectDump(d),
      ),
    });
  }

  private buildAreasPanel(
    trace: Trace,
    proc: ProcMemStat,
    triage: ProcessTriageBundle | undefined,
    hasSmaps: boolean,
    hasNative: boolean,
    hasJavaCapture: boolean,
    selTs: time | undefined,
    openDomain: (key: DomainPillKey, view?: NavView) => void,
  ): m.Children {
    const total = triage?.totalSmapsBytes ?? 0;
    const nativeBytes = hasSmaps
      ? (triage?.nativeRssSwapBytes ?? 0) > 0
        ? triage!.nativeRssSwapBytes
        : (triage?.heapprofdUnreleasedBytes ?? 0)
      : hasNative
        ? (triage?.heapprofdUnreleasedBytes ?? 0)
        : undefined;
    const javaBytes = hasSmaps
      ? (triage?.javaRssSwapBytes ?? 0) > 0
        ? triage!.javaRssSwapBytes
        : (triage?.javaHeapBytes ?? 0)
      : hasJavaCapture
        ? (triage?.javaHeapBytes ?? 0)
        : undefined;
    const fileBytes = hasSmaps ? (triage?.fileSmapsBytes ?? 0) : undefined;
    const gfxBytes = hasSmaps
      ? (triage?.graphicsSmapsBytes ?? 0) > 0
        ? triage!.graphicsSmapsBytes
        : (triage?.gfxCounterBytes ?? 0)
      : (triage?.gfxCounterBytes ?? 0) > 0
        ? triage!.gfxCounterBytes
        : undefined;
    const stackBytes = hasSmaps ? (triage?.stackSmapsBytes ?? 0) : undefined;
    const otherBytes = triage?.otherSmapsBytes ?? 0;

    const nativeGrowing =
      (triage?.nativeDeltaBytes ?? 0) > 0 &&
      (triage?.nativeDeltaBytes ?? 0) >= (triage?.javaDeltaBytes ?? 0);

    const areas: MemoryAreaCardModel[] = [
      {
        id: 'native',
        label: 'Native',
        color: '#3b82f6',
        bytes: nativeBytes,
        totalBytes: total,
        deltaBytes: triage?.nativeDeltaBytes,
        hasDeepProfiler: hasNative,
        primaryHighlight: hasNative || nativeGrowing,
        summary: hasNative
          ? `heapprofd captured (${formatBytesIec(triage?.heapprofdUnreleasedBytes ?? 0)} unreleased across ${proc.nativeDumps} ${proc.nativeDumps === 1 ? 'dump' : 'dumps'}) — explore malloc/new callstacks, .so libraries, and churn.`
          : (nativeBytes ?? 0) > 0
            ? 'Physical smaps native allocator arenas (scudo / libc_malloc / [heap]). No heapprofd profile recorded.'
            : 'No native heap profile or smaps native arenas recorded.',
        onOpen: () => openDomain('native'),
        quickLinks: hasNative
          ? [
              {
                label: 'Flamegraph',
                icon: 'local_fire_department',
                onclick: () => openDomain('native', 'native-flamegraph'),
              },
              {
                label: 'Libraries & Callsites',
                icon: 'library_books',
                onclick: () => openDomain('native', 'native-callsites'),
              },
              {
                label: 'Growth & Churn',
                icon: 'trending_up',
                onclick: () => openDomain('native', 'native-growth'),
              },
            ]
          : undefined,
      },
      {
        id: 'java',
        label: 'Java',
        color: '#e59500',
        bytes: javaBytes,
        totalBytes: total,
        deltaBytes: triage?.javaDeltaBytes,
        hasDeepProfiler: hasJavaCapture,
        primaryHighlight: hasJavaCapture && !hasNative && !nativeGrowing,
        summary: hasJavaCapture
          ? `${proc.heapDumps} Java ${proc.heapDumps === 1 ? 'heap dump' : 'heap dumps'}${triage && triage.javaHeapBytes > 0 ? ` (${formatBytesIec(triage.javaHeapBytes)} reachable live heap)` : ''} — inspect Dominators, Classes, Bitmaps, Strings, and GC roots.`
          : (javaBytes ?? 0) > 0
            ? 'Physical smaps ART/Dalvik spaces. No java_hprof dump recorded.'
            : 'No Java heap dump recorded.',
        onOpen: () => openDomain('java'),
        quickLinks: hasJavaCapture
          ? [
              {
                label: 'Flamegraph',
                icon: 'local_fire_department',
                onclick: () => openDomain('java', 'flamegraph'),
              },
              {
                label: 'Dominators',
                icon: 'account_tree',
                onclick: () => openDomain('java', 'dominators'),
              },
              {
                label: 'Classes',
                icon: 'category',
                onclick: () => openDomain('java', 'classes'),
              },
              {
                label: 'Bitmaps',
                icon: 'image',
                onclick: () => openDomain('java', 'bitmaps'),
              },
            ]
          : undefined,
      },
      {
        id: 'file',
        label: 'File-backed',
        color: '#2e9e4c',
        bytes: fileBytes,
        totalBytes: total,
        deltaBytes: triage?.fileDeltaBytes,
        hasDeepProfiler: false,
        summary:
          (fileBytes ?? 0) > 0
            ? 'Mapped .oat/.vdex/.art Java code, .so native libraries, APKs & resources — separates evictable clean pages from private dirty cost.'
            : 'No file-backed smaps recorded.',
        onOpen: () => openDomain('file'),
      },
      {
        id: 'graphics',
        label: 'Graphics',
        color: '#9333ea',
        bytes: gfxBytes,
        totalBytes: total,
        deltaBytes: triage?.graphicsDeltaBytes,
        hasDeepProfiler: false,
        summary:
          (gfxBytes ?? 0) > 0
            ? 'GPU driver mappings (/dev/kgsl, /dev/mali, /dev/dri) and DMA-BUF surfaces.'
            : hasJavaCapture
              ? 'No GPU / DMA-BUF mappings recorded (see Java → Bitmaps for pixel buffers).'
              : 'No GPU / DMA-BUF mappings recorded.',
        onOpen: () => openDomain('graphics'),
      },
      {
        id: 'stack',
        label: 'Thread stacks',
        color: '#0891b2',
        bytes: stackBytes,
        totalBytes: total,
        deltaBytes: triage?.stackDeltaBytes,
        hasDeepProfiler: false,
        summary:
          (triage?.threadCount ?? 0) > 0
            ? `${triage!.threadCount} ${triage!.threadCount === 1 ? 'thread' : 'threads'} recorded · stack VMAs and thread-pool grouping.`
            : 'Thread stack VMAs and thread-pool breakdown.',
        onOpen: () => openDomain('stack'),
      },
      ...(otherBytes > 0
        ? [
            {
              id: 'other',
              label: 'Other',
              color: '#6b7280',
              bytes: otherBytes,
              totalBytes: total,
              deltaBytes: triage?.otherDeltaBytes,
              hasDeepProfiler: false,
              summary: 'Uncategorized anonymous and special kernel VMAs.',
              onOpen: () => openDomain('other'),
            } satisfies MemoryAreaCardModel,
          ]
        : []),
    ];

    return m(MemoryAreasPanel, {
      trace,
      upid: proc.upid,
      hasSmaps,
      hasNative,
      hasJava: hasJavaCapture,
      selTs,
      areas,
      onOpenDomain: (id: MemoryAreaId) => openDomain(id),
    });
  }

  private renderDefaultChangedHint(attrs: MemoryExplorerPageAttrs): m.Children {
    const {trace, autoNavigated, openByDefault, hideDefaultChangedHint} = attrs;
    // The hint explains a ui.perfetto.dev default; a product has no other one.
    if (
      trace.productId !== undefined ||
      !autoNavigated ||
      !openByDefault.get() ||
      hideDefaultChangedHint.get()
    ) {
      return undefined;
    }
    return m(
      Callout,
      {
        className: 'pf-memscope-default-page-callout',
        icon: 'info',
        intent: Intent.Primary,
      },
      m('.pf-memscope-default-page-callout__body', [
        m(
          'span.pf-memscope-default-page-callout__message',
          'Memory Explorer is now the default page when opening traces with ' +
            'memory data.',
        ),
        m(
          '.pf-memscope-default-page-callout__actions',
          m(
            ButtonGroup,
            {className: 'pf-memscope-default-page-callout__split-button'},
            m(Button, {
              label: 'Back to timeline',
              icon: 'arrow_back',
              variant: ButtonVariant.Filled,
              onclick: () => trace.navigate('#!/viewer'),
            }),
            m(
              PopupMenu,
              {
                trigger: m(Button, {
                  icon: 'arrow_drop_down',
                  variant: ButtonVariant.Filled,
                  title: 'More options',
                }),
                position: PopupPosition.BottomEnd,
              },
              m(MenuItem, {
                label: 'Never open this page by default',
                icon: 'timeline',
                onclick: () => openByDefault.set(false),
              }),
              m(MenuItem, {
                label: 'Dismiss forever',
                icon: 'close',
                onclick: () => hideDefaultChangedHint.set(true),
              }),
            ),
          ),
        ),
      ]),
    );
  }
}

function captureSummary(p: ProcMemStat): string {
  const parts: string[] = [];
  if (p.heapDumps > 0) parts.push(`${p.heapDumps} java_hprof`);
  if (p.nativeDumps > 0) parts.push(`${p.nativeDumps} heapprofd`);
  if (p.smapsSnapshots > 0) parts.push(`${p.smapsSnapshots} smaps`);
  return parts.join(', ');
}
