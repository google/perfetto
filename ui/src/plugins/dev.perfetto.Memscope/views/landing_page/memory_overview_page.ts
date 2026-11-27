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
import {AsyncMemo} from '../../../../base/async_memo';
import {classNames} from '../../../../base/classnames';
import {Icons} from '../../../../base/semantic_icons';
import {Time, type time} from '../../../../base/time';
import {formatDuration} from '../../../../components/time_utils';
import type {Setting} from '../../../../public/settings';
import type {Trace} from '../../../../public/trace';
import {LONG, NUM_NULL} from '../../../../trace_processor/query_result';
import {Anchor} from '../../../../widgets/anchor';
import {Button, ButtonGroup, ButtonVariant} from '../../../../widgets/button';
import {Chip} from '../../../../widgets/chip';
import {Intent} from '../../../../widgets/common';
import {EmptyState} from '../../../../widgets/empty_state';
import {Icon} from '../../../../widgets/icon';
import {MenuItem, PopupMenu} from '../../../../widgets/menu';
import {PopupPosition} from '../../../../widgets/popup';
import {HeapDumpContentView} from '../../../com.android.HeapDumpExplorer/heap_dump_page';
import type {NavView} from '../../../com.android.HeapDumpExplorer/nav_state';
import type {HeapDumpExplorerSession} from '../../../com.android.HeapDumpExplorer/session';
import type {HeapDump} from '../../../com.android.HeapDumpExplorer/types';
import {
  loadOomeDetails,
  OOME_DETAILS_TITLE,
  type OomeDetails,
  renderOomeDetailsGrid,
} from '../../../dev.perfetto.HeapProfile/oome_callstack_common';
import {Callout} from '../../components/callout';
import {ColorChip} from '../../components/color_chip';
import {Panel} from '../../components/panel';
import {oomBucketLabel, oomScoreBucket} from '../../process_data';
import {formatBytes} from './mem_format';
import {MemoryTimelineHero} from './memory_timeline_hero';
import {type CaptureInfo, loadCaptureInfo} from './proc_mem_overview';
import {
  loadProcessMemoryStats,
  type ProcMemStat,
  type ProcWithMem,
} from './proc_mem_stats';
import {type MemSelection, nearestByTs} from './selection';
import {SmapsDetail} from './smaps_detail';
import {BitmapsSection} from './summary/bitmaps_section';
import {JavaSection} from './summary/java_section';
import {MemoryMap} from './summary/memory_map';
import {NativeFlamegraphView} from './summary/native_flamegraph';
import {NativeSection} from './summary/native_section';
import {TraceOverview} from './summary/trace_overview';
import './memory_overview_page.scss';

export type MemoryTreePage =
  | 'overview'
  | 'smaps'
  | 'java-overview'
  | 'java-bitmaps'
  | 'java-hde'
  | 'native-overview'
  | 'native-flamegraph';

export interface MemoryOverviewPageAttrs {
  readonly trace: Trace;
  readonly upid?: number;
  readonly activePage: MemoryTreePage;
  readonly hdeSession?: HeapDumpExplorerSession;
  readonly hdeSubpage?: string;
  readonly autoNavigated: boolean;
  readonly openByDefault: Setting<boolean>;
  readonly hideDefaultChangedHint: Setting<boolean>;
  readonly onUpidChange: (upid: number) => void;
  readonly onNavigatePage: (page: MemoryTreePage, hdeSubpage?: string) => void;
  readonly onOpenJavaHeap: (
    ts: time,
    view: NavView,
    params?: Record<string, unknown>,
  ) => void;
}

const MAX_INLINE_DUMP_CHIPS = 4;

interface StaticHdeTreeItem {
  readonly view: NavView;
  readonly label: string;
  readonly icon: string;
}

const HDE_JAVA_TREE_ITEMS: ReadonlyArray<StaticHdeTreeItem> = [
  {view: 'flamegraph', label: 'Flamegraph', icon: 'local_fire_department'},
  {view: 'classes', label: 'Classes', icon: 'category'},
  {view: 'objects', label: 'Objects', icon: 'data_object'},
  {view: 'dominators', label: 'Dominators', icon: 'account_tree'},
  {view: 'strings', label: 'Strings', icon: 'text_fields'},
  {view: 'arrays', label: 'Arrays', icon: 'view_array'},
  {view: 'callstack', label: 'Callstack', icon: 'format_list_numbered'},
];

function formatOffsetSec(trace: Trace, ts: time): string {
  return `${(Number(Time.diff(ts, trace.traceInfo.start)) / 1e9).toFixed(1)}s`;
}

function formatUptimeShort(ns: number): string {
  const secs = Math.max(0, ns) / 1e9;
  if (secs < 60) return `${secs.toFixed(0)}s`;
  const mins = secs / 60;
  if (mins < 60) return `${Math.floor(mins)}m ${Math.round(secs % 60)}s`;
  return `${Math.floor(mins / 60)}h ${Math.round(mins % 60)}m`;
}

interface ProcessHeaderMeta {
  readonly uptimeNs?: number;
  readonly oomScoreAdj?: number;
  readonly dmabufRssSize?: number;
  readonly oome?: OomeDetails;
  readonly oomeDumpTs?: time;
}

async function loadProcessHeaderMeta(
  trace: Trace,
  upid: number,
): Promise<ProcessHeaderMeta> {
  try {
    await trace.engine.query(
      'INCLUDE PERFETTO MODULE android.memory.heap_graph.heap_graph_stats;',
    );
    const res = await trace.engine.query(`
      SELECT
        s.graph_sample_ts AS dump_ts,
        s.process_uptime AS uptime,
        s.oom_score_adj AS oom_score_adj,
        s.dmabuf_rss_size AS dmabuf_rss_size,
        s.is_out_of_memory_error AS is_oome
      FROM android_heap_graph_stats s
      WHERE s.upid = ${upid}
      ORDER BY s.graph_sample_ts DESC
      LIMIT 1
    `);
    const it = res.iter({
      dump_ts: LONG,
      uptime: NUM_NULL,
      oom_score_adj: NUM_NULL,
      dmabuf_rss_size: NUM_NULL,
      is_oome: NUM_NULL,
    });
    if (!it.valid()) return {};
    const dumpTs = Time.fromRaw(it.dump_ts);
    const uptimeNs = it.uptime ?? undefined;
    const oomScoreAdj = it.oom_score_adj ?? undefined;
    const dmabufRssSize = it.dmabuf_rss_size ?? undefined;
    const isOome = it.is_oome !== null && it.is_oome !== 0;
    let oome: OomeDetails | undefined;
    if (isOome) {
      try {
        oome = await loadOomeDetails(trace.engine, dumpTs);
      } catch {
        // OOME module or table unavailable.
      }
    }
    return {
      uptimeNs,
      oomScoreAdj,
      dmabufRssSize,
      oome,
      oomeDumpTs: oome !== undefined ? dumpTs : undefined,
    };
  } catch {
    return {};
  }
}

export class MemoryOverviewPage implements m.ClassComponent<MemoryOverviewPageAttrs> {
  private readonly procsSlot = new AsyncMemo<ProcWithMem>();
  private readonly captureSlot = new AsyncMemo<CaptureInfo>();
  private readonly headerMetaSlot = new AsyncMemo<ProcessHeaderMeta>();
  private readonly selectionByUpid = new Map<number, MemSelection>();
  private javaExpanded = true;
  private nativeExpanded = true;

  onremove() {
    this.procsSlot.dispose();
    this.captureSlot.dispose();
    this.headerMetaSlot.dispose();
  }

  view({attrs}: m.Vnode<MemoryOverviewPageAttrs>) {
    const {
      trace,
      upid,
      activePage,
      hdeSession,
      hdeSubpage,
      autoNavigated,
      openByDefault,
      hideDefaultChangedHint,
      onUpidChange,
      onNavigatePage,
      onOpenJavaHeap,
    } = attrs;

    const procs = this.procsSlot.use({
      key: trace.traceInfo.uuid,
      compute: () => loadProcessMemoryStats(trace.engine),
    }).data;

    if (procs === undefined) {
      return m(
        '.pf-memscope-page.pf-memtree-page',
        m(
          '.pf-memtl-page__empty',
          m(EmptyState, {icon: 'hourglass', title: 'Loading processes...'}),
        ),
      );
    }

    if (procs.length === 0) {
      return m(
        '.pf-memscope-page.pf-memtree-page',
        m(
          '.pf-memtl-page__empty',
          m(EmptyState, {
            icon: 'memory',
            title: 'No processes with memory data in this trace',
          }),
        ),
      );
    }

    const validUpid =
      upid !== undefined && !Number.isNaN(upid) ? upid : undefined;
    const selectedProc =
      validUpid !== undefined
        ? procs.find((p) => p.upid === validUpid)
        : undefined;

    let capture: CaptureInfo | undefined;
    let captureError: string | undefined;
    let headerMeta: ProcessHeaderMeta | undefined;
    if (validUpid !== undefined) {
      try {
        capture = this.captureSlot.use({
          key: {traceId: trace.traceInfo.uuid, upid: validUpid},
          compute: () => loadCaptureInfo(trace, validUpid),
        }).data;
      } catch (e) {
        captureError = String(e);
      }
      headerMeta = this.headerMetaSlot.use({
        key: {traceId: trace.traceInfo.uuid, upid: validUpid},
        compute: () => loadProcessHeaderMeta(trace, validUpid),
      }).data;
    }

    const procDumps =
      validUpid !== undefined && hdeSession !== undefined
        ? hdeSession.getDumpsForProcess(validUpid)
        : [];
    const currentSelection =
      validUpid !== undefined ? this.selectionByUpid.get(validUpid) : undefined;

    // Keep HDE route state synced before reading active nav/tabs for the tree.
    if (activePage === 'java-hde' && hdeSession !== undefined) {
      hdeSession.syncFromSubpage(hdeSubpage);
      hdeSession.syncInstanceTabFromNav();
      hdeSession.syncFlamegraphTabFromNav();
    }

    const smapsAvailable = capture === undefined || capture.smaps.samples > 0;
    const dumpsAvailable =
      procDumps.length > 0 || (capture !== undefined && capture.dumps > 0);
    const nativeAvailable = capture === undefined || capture.native.samples > 0;

    const handleSelectTimeline = (next: MemSelection | undefined) => {
      if (validUpid === undefined) return;
      if (next === undefined) {
        this.selectionByUpid.delete(validUpid);
      } else {
        this.selectionByUpid.set(validUpid, next);
      }
      // Keep HDE's active dump synchronized with the timeline's target point.
      if (hdeSession !== undefined && procDumps.length > 0) {
        hdeSession.selectDumpForProcess(validUpid, next?.sel);
      }
    };

    const handleSelectDump = (dump: HeapDump) => {
      hdeSession?.selectDump(dump);
      if (validUpid !== undefined) {
        const cur = this.selectionByUpid.get(validUpid);
        if (cur?.base !== undefined) {
          this.selectionByUpid.set(validUpid, {base: cur.base, sel: dump.ts});
        } else {
          this.selectionByUpid.set(validUpid, {sel: dump.ts});
        }
      }
    };

    const handleOpenHdeView = (
      view: NavView,
      params?: Record<string, unknown>,
    ) => {
      if (validUpid === undefined) return;
      if (hdeSession !== undefined && procDumps.length > 0) {
        hdeSession.selectDumpForProcess(validUpid, currentSelection?.sel);
        hdeSession.navigateWithTabs(view, params);
      } else {
        onNavigatePage('java-hde', view);
      }
    };

    return m('.pf-memscope-page.pf-memtree-page', [
      // Top Fixed Chrome: Process Bar + Hints + Hero Memory Timeline
      m('.pf-memtree-header', [
        this.renderTopBar(
          trace,
          procs,
          selectedProc,
          validUpid,
          capture,
          headerMeta,
          onUpidChange,
        ),
        this.renderDefaultChangedHint(
          trace,
          autoNavigated,
          openByDefault,
          hideDefaultChangedHint,
        ),
        captureError !== undefined && m('p.pf-error', `Error: ${captureError}`),
        validUpid !== undefined &&
          captureError === undefined &&
          m(MemoryTimelineHero, {
            trace,
            upid: validUpid,
            procDumps,
            activeDump: hdeSession?.activeDump ?? null,
            selection: currentSelection,
            onSelect: handleSelectTimeline,
            onSelectDump: (dump) => {
              hdeSession?.selectDump(dump);
            },
          }),
      ]),

      // Main Split Body: Left Tree Sidebar + Synced Right Page View
      validUpid === undefined
        ? m(
            '.pf-memtl-page__empty',
            m(EmptyState, {
              icon: 'error_outline',
              title: 'Unable to resolve process from URL',
            }),
          )
        : captureError === undefined &&
          m('.pf-memtree-workspace', [
            this.renderTreeSidebar({
              trace,
              activePage,
              hdeSession,
              procDumps,
              smapsAvailable,
              dumpsAvailable,
              nativeAvailable,
              selection: currentSelection,
              onClearSelection: () => handleSelectTimeline(undefined),
              onNavigatePage,
              onOpenHdeView: handleOpenHdeView,
            }),
            this.renderMainContent({
              trace,
              upid: validUpid,
              activePage,
              hdeSession,
              hdeSubpage,
              procDumps,
              headerMeta,
              selection: currentSelection,
              onNavigatePage,
              onOpenJavaHeap,
              onOpenHdeView: handleOpenHdeView,
              onSelectDump: handleSelectDump,
            }),
          ]),
    ]);
  }

  private renderTreeSidebar(opts: {
    readonly trace: Trace;
    readonly activePage: MemoryTreePage;
    readonly hdeSession?: HeapDumpExplorerSession;
    readonly procDumps: ReadonlyArray<HeapDump>;
    readonly smapsAvailable: boolean;
    readonly dumpsAvailable: boolean;
    readonly nativeAvailable: boolean;
    readonly selection?: MemSelection;
    readonly onClearSelection: () => void;
    readonly onNavigatePage: (
      page: MemoryTreePage,
      hdeSubpage?: string,
    ) => void;
    readonly onOpenHdeView: (
      view: NavView,
      params?: Record<string, unknown>,
    ) => void;
  }): m.Children {
    const {
      trace,
      activePage,
      hdeSession,
      procDumps,
      smapsAvailable,
      dumpsAvailable,
      nativeAvailable,
      selection,
      onClearSelection,
      onNavigatePage,
      onOpenHdeView,
    } = opts;

    const hdeNav = hdeSession?.nav;
    const isHdeActive = activePage === 'java-hde';
    const activeHdeView = isHdeActive ? hdeNav?.view : undefined;
    const activeFg =
      isHdeActive && hdeSession !== undefined
        ? hdeSession.activeFlamegraph
        : null;
    const activeInstId =
      isHdeActive && hdeSession !== undefined
        ? hdeSession.activeInstanceObjId
        : null;

    const treeItem = (item: {
      readonly key?: string;
      readonly label: string;
      readonly icon: string;
      readonly active: boolean;
      readonly nested?: boolean;
      readonly muted?: boolean;
      readonly onClose?: () => void;
      readonly onclick: () => void;
    }) =>
      m(
        '.pf-memtree-node',
        {
          ...(item.key !== undefined ? {key: item.key} : {}),
          className: classNames(
            item.nested && 'pf-memtree-node--nested',
            item.active && 'pf-memtree-node--active',
            item.muted && 'pf-memtree-node--muted',
          ),
        },
        [
          m(
            'button.pf-memtree-node__btn',
            {type: 'button', title: item.label, onclick: item.onclick},
            [
              m(Icon, {icon: item.icon, className: 'pf-memtree-node__icon'}),
              m('span.pf-memtree-node__label', item.label),
            ],
          ),
          item.onClose !== undefined &&
            m(
              'button.pf-memtree-node__close',
              {
                type: 'button',
                title: `Close ${item.label}`,
                onclick: (e: Event) => {
                  e.stopPropagation();
                  item.onClose?.();
                },
              },
              m(Icon, {icon: 'close'}),
            ),
        ],
      );

    const fgTabs = hdeSession?.flamegraphTabs ?? [];
    const instTabs = hdeSession?.instanceTabs ?? [];

    return m('aside.pf-memtree-sidebar', {'aria-label': 'Memory navigation'}, [
      // Active timeline scope banner inside the sidebar when pinned/diffing
      selection !== undefined &&
        m(
          '.pf-memtree-scope',
          {
            className: classNames(
              selection.base !== undefined && 'pf-memtree-scope--delta',
            ),
          },
          [
            m(Icon, {
              icon:
                selection.base !== undefined
                  ? 'compare_arrows'
                  : 'photo_camera',
            }),
            m(
              'span.pf-memtree-scope__text',
              selection.base !== undefined
                ? `Δ ${formatOffsetSec(trace, selection.base)} → ${formatOffsetSec(trace, selection.sel)}`
                : `Pinned @ ${formatOffsetSec(trace, selection.sel)}`,
            ),
            m(
              'button.pf-memtree-scope__clear',
              {
                type: 'button',
                title: 'Clear timeline selection',
                onclick: onClearSelection,
              },
              m(Icon, {icon: 'close'}),
            ),
          ],
        ),

      m('nav.pf-memtree-nav', [
        // Top-level Overview (Memory Breakdown) & Smaps (VMAs)
        treeItem({
          label: 'Overview',
          icon: 'donut_large',
          active: activePage === 'overview',
          onclick: () => onNavigatePage('overview'),
        }),
        treeItem({
          label: 'Smaps (VMAs)',
          icon: 'table_rows',
          active: activePage === 'smaps',
          muted: !smapsAvailable,
          onclick: () => onNavigatePage('smaps'),
        }),

        // Java Branch
        m('.pf-memtree-group', [
          m(
            'button.pf-memtree-group__header',
            {
              type: 'button',
              className: classNames(
                !dumpsAvailable && 'pf-memtree-group__header--muted',
              ),
              onclick: () => {
                this.javaExpanded = !this.javaExpanded;
              },
            },
            [
              m(Icon, {
                icon: this.javaExpanded ? 'expand_more' : 'chevron_right',
                className: 'pf-memtree-group__caret',
              }),
              m(Icon, {icon: 'coffee', className: 'pf-memtree-group__icon'}),
              m('span.pf-memtree-group__label', 'Java'),
            ],
          ),
          this.javaExpanded &&
            m('.pf-memtree-group__children', [
              treeItem({
                label: 'Overview',
                icon: 'dashboard',
                nested: true,
                active: activePage === 'java-overview',
                muted: !dumpsAvailable,
                onclick: () => onNavigatePage('java-overview'),
              }),
              treeItem({
                label: 'Bitmaps',
                icon: 'photo_library',
                nested: true,
                active:
                  activePage === 'java-bitmaps' || activeHdeView === 'bitmaps',
                muted: !dumpsAvailable,
                onclick: () => onNavigatePage('java-bitmaps'),
              }),
              HDE_JAVA_TREE_ITEMS.map((item) =>
                treeItem({
                  key: `hde-${item.view}`,
                  label: item.label,
                  icon: item.icon,
                  nested: true,
                  active: activeHdeView === item.view,
                  muted: procDumps.length === 0,
                  onclick: () => onOpenHdeView(item.view),
                }),
              ),
              hdeSession?.hasFieldValues &&
                treeItem({
                  label: 'Duplicates',
                  icon: 'content_copy',
                  nested: true,
                  active: activeHdeView === 'overview',
                  muted: procDumps.length === 0,
                  onclick: () => onOpenHdeView('overview'),
                }),
              // Dynamic HDE Flamegraph Objects drill-down items
              fgTabs.map((fg) =>
                treeItem({
                  key: `fg-${fg.isDominator ? 'd' : 'n'}:${fg.pathHashes}`,
                  label:
                    fg.count !== null
                      ? `Objects (${fg.count.toLocaleString()})`
                      : 'Flamegraph objects',
                  icon: 'filter_list',
                  nested: true,
                  active:
                    activeFg !== null &&
                    activeFg.pathHashes === fg.pathHashes &&
                    activeFg.isDominator === fg.isDominator,
                  onClose: () =>
                    hdeSession?.closeFlamegraph(fg.pathHashes, fg.isDominator),
                  onclick: () =>
                    onOpenHdeView('flamegraph-objects', {
                      pathHashes: fg.pathHashes,
                      isDominator: fg.isDominator,
                    }),
                }),
              ),
              // Dynamic HDE Instance inspector items
              instTabs.map((inst) =>
                treeItem({
                  key: `inst-${inst.objId}`,
                  label: inst.label,
                  icon: 'data_object',
                  nested: true,
                  active: activeInstId === inst.objId,
                  onClose: () => hdeSession?.closeInstanceTab(inst.objId),
                  onclick: () =>
                    onOpenHdeView('object', {
                      id: inst.objId,
                      label: inst.label,
                    }),
                }),
              ),
            ]),
        ]),

        // Native Branch
        m('.pf-memtree-group', [
          m(
            'button.pf-memtree-group__header',
            {
              type: 'button',
              className: classNames(
                !nativeAvailable && 'pf-memtree-group__header--muted',
              ),
              onclick: () => {
                this.nativeExpanded = !this.nativeExpanded;
              },
            },
            [
              m(Icon, {
                icon: this.nativeExpanded ? 'expand_more' : 'chevron_right',
                className: 'pf-memtree-group__caret',
              }),
              m(Icon, {
                icon: 'memory_alt',
                className: 'pf-memtree-group__icon',
              }),
              m('span.pf-memtree-group__label', 'Native'),
            ],
          ),
          this.nativeExpanded &&
            m('.pf-memtree-group__children', [
              treeItem({
                label: 'Overview',
                icon: 'dashboard',
                nested: true,
                active: activePage === 'native-overview',
                muted: !nativeAvailable,
                onclick: () => onNavigatePage('native-overview'),
              }),
              treeItem({
                label: 'Flamegraph',
                icon: 'local_fire_department',
                nested: true,
                active: activePage === 'native-flamegraph',
                muted: !nativeAvailable,
                onclick: () => onNavigatePage('native-flamegraph'),
              }),
            ]),
        ]),
      ]),
    ]);
  }

  private renderMainContent(opts: {
    readonly trace: Trace;
    readonly upid: number;
    readonly activePage: MemoryTreePage;
    readonly hdeSession?: HeapDumpExplorerSession;
    readonly hdeSubpage?: string;
    readonly procDumps: ReadonlyArray<HeapDump>;
    readonly headerMeta?: ProcessHeaderMeta;
    readonly selection?: MemSelection;
    readonly onNavigatePage: (
      page: MemoryTreePage,
      hdeSubpage?: string,
    ) => void;
    readonly onOpenJavaHeap: (
      ts: time,
      view: NavView,
      params?: Record<string, unknown>,
    ) => void;
    readonly onOpenHdeView: (
      view: NavView,
      params?: Record<string, unknown>,
    ) => void;
    readonly onSelectDump: (dump: HeapDump) => void;
  }): m.Children {
    const {
      trace,
      upid,
      activePage,
      hdeSession,
      hdeSubpage,
      procDumps,
      headerMeta,
      selection,
      onNavigatePage,
      onOpenJavaHeap,
      onOpenHdeView,
      onSelectDump,
    } = opts;

    const isBitmapsGallery =
      activePage === 'java-hde' && hdeSession?.nav.view === 'bitmaps';

    // Full-height explorer views (HDE views + Native Flamegraph) fill 100% of
    // the right pane without double scrollbars.
    if (activePage === 'native-flamegraph') {
      return m(
        'main.pf-memtree-main.pf-memtree-main--fill',
        m(NativeFlamegraphView, {
          trace,
          upid,
          selTs: selection?.sel,
          baseTs: selection?.base,
        }),
      );
    }

    if (activePage === 'java-hde') {
      if (hdeSession === undefined || procDumps.length === 0) {
        return m(
          'main.pf-memtree-main.pf-memtree-main--scroll',
          m(
            '.pf-memtree-empty',
            m(
              EmptyState,
              {
                icon: 'account_tree',
                title: 'No Java heap dumps for this process',
              },
              'Capture a java_hprof dump to inspect Java flamegraphs, classes, ' +
                'objects, dominators, bitmaps, and strings.',
            ),
          ),
        );
      }

      return m('main.pf-memtree-main.pf-memtree-main--fill', [
        isBitmapsGallery &&
          this.renderBitmapsModeBar({
            mode: 'gallery',
            onSummary: () => onNavigatePage('java-bitmaps'),
            onGallery: () => onOpenHdeView('bitmaps'),
          }),
        this.renderHdeDumpBar(
          trace,
          procDumps,
          hdeSession.activeDump,
          selection,
          onSelectDump,
        ),
        m(HeapDumpContentView, {
          session: hdeSession,
          subpage: hdeSubpage,
          active: true,
        }),
      ]);
    }

    // Scrollable panel-based pages: Overview, Smaps, Java Overview, Java
    // Bitmaps (Summary), Native Overview.
    return m('main.pf-memtree-main.pf-memtree-main--scroll', [
      activePage === 'overview' &&
        m('.pf-memtree-stack', [
          headerMeta?.oome !== undefined &&
            this.renderOomeBanner(
              headerMeta.oome,
              headerMeta.oomeDumpTs,
              procDumps.length > 0 ? onOpenJavaHeap : undefined,
            ),
          m(TraceOverview, {trace, upid}),
          m(MemoryMap, {
            trace,
            upid,
            selTs: selection?.sel,
            baseTs: selection?.base,
            onInspectSmaps: () => onNavigatePage('smaps'),
          }),
        ]),

      activePage === 'smaps' &&
        m(
          '.pf-memtree-stack',
          m(SmapsDetail, {
            trace,
            upid,
            selTs: selection?.sel,
            baseTs: selection?.base,
            hideSnapshotPicker: true,
          }),
        ),

      activePage === 'java-overview' &&
        m('.pf-memtree-stack', [
          headerMeta?.oome !== undefined &&
            this.renderOomeBanner(
              headerMeta.oome,
              headerMeta.oomeDumpTs,
              procDumps.length > 0 ? onOpenJavaHeap : undefined,
            ),
          m(JavaSection, {
            trace,
            upid,
            selTs: selection?.sel,
            baseTs: selection?.base,
            onOpenJavaHeap: procDumps.length > 0 ? onOpenJavaHeap : undefined,
          }),
        ]),

      activePage === 'java-bitmaps' &&
        m('.pf-memtree-stack', [
          procDumps.length > 0 &&
            this.renderBitmapsModeBar({
              mode: 'summary',
              onSummary: () => onNavigatePage('java-bitmaps'),
              onGallery: () => onOpenHdeView('bitmaps'),
            }),
          m(BitmapsSection, {
            trace,
            upid,
            selTs: selection?.sel,
            baseTs: selection?.base,
            onOpenBitmapsGallery:
              procDumps.length > 0
                ? (ts: time, filterKey?: string) =>
                    onOpenJavaHeap(
                      ts,
                      'bitmaps',
                      filterKey !== undefined ? {filterKey} : undefined,
                    )
                : undefined,
          }),
        ]),

      activePage === 'native-overview' &&
        m(
          '.pf-memtree-stack',
          m(NativeSection, {
            trace,
            upid,
            selTs: selection?.sel,
            baseTs: selection?.base,
            onOpenFlamegraph: () => onNavigatePage('native-flamegraph'),
          }),
        ),
    ]);
  }

  private renderBitmapsModeBar(opts: {
    readonly mode: 'summary' | 'gallery';
    readonly onSummary: () => void;
    readonly onGallery: () => void;
  }): m.Children {
    return m('.pf-memtree-subbar', [
      m('span.pf-memtree-subbar__title', 'Bitmaps'),
      m(ButtonGroup, [
        m(Button, {
          label: 'Summary & Dimensions',
          icon: 'analytics',
          compact: true,
          variant: ButtonVariant.Outlined,
          active: opts.mode === 'summary',
          onclick: opts.onSummary,
        }),
        m(Button, {
          label: 'Bitmap Gallery',
          icon: 'photo_library',
          compact: true,
          variant: ButtonVariant.Outlined,
          active: opts.mode === 'gallery',
          onclick: opts.onGallery,
        }),
      ]),
    ]);
  }

  private renderHdeDumpBar(
    trace: Trace,
    procDumps: ReadonlyArray<HeapDump>,
    activeDump: HeapDump | null,
    selection: MemSelection | undefined,
    onSelectDump: (dump: HeapDump) => void,
  ): m.Children {
    const baseDump =
      selection?.base !== undefined
        ? nearestByTs(procDumps, selection.base)
        : undefined;
    const selDump =
      selection !== undefined
        ? nearestByTs(procDumps, selection.sel)
        : (activeDump ?? undefined);
    const rangeSpansTwoDumps =
      baseDump !== undefined &&
      selDump !== undefined &&
      baseDump.ts !== selDump.ts;

    if (procDumps.length <= 1 && !rangeSpansTwoDumps) {
      return null;
    }

    return m('.pf-memtree-subbar.pf-memtree-subbar--dumps', [
      m('span.pf-memtree-subbar__label', 'Heap dump:'),
      procDumps.length <= MAX_INLINE_DUMP_CHIPS
        ? m(
            '.pf-memtl-heap-card__dump-chips',
            procDumps.map((d, i) =>
              m(Chip, {
                key: `${d.upid}:${d.ts.toString()}`,
                label: `#${i + 1} · ${formatOffsetSec(trace, d.ts)}`,
                rounded: true,
                interactive: true,
                intent: d === activeDump ? Intent.Primary : Intent.None,
                onclick: () => onSelectDump(d),
              }),
            ),
          )
        : m(
            PopupMenu,
            {
              position: PopupPosition.BottomStart,
              trigger: m(Button, {
                label:
                  activeDump !== null
                    ? `Dump #${procDumps.indexOf(activeDump) + 1}/${procDumps.length} · ${formatOffsetSec(trace, activeDump.ts)}`
                    : `${procDumps.length} dumps`,
                icon: 'photo_camera',
                rightIcon: 'arrow_drop_down',
                variant: ButtonVariant.Outlined,
                compact: true,
              }),
            },
            procDumps.map((d, i) =>
              m(MenuItem, {
                label: `Dump #${i + 1} — ${formatDuration(trace, Time.diff(d.ts, trace.traceInfo.start))}`,
                active: d === activeDump,
                onclick: () => onSelectDump(d),
              }),
            ),
          ),
      rangeSpansTwoDumps &&
        m(
          'span.pf-memtree-subbar__hint',
          `Showing target Dump #${procDumps.indexOf(selDump) + 1}; see Java → Overview for Δ vs Dump #${procDumps.indexOf(baseDump) + 1}.`,
        ),
    ]);
  }

  private renderTopBar(
    trace: Trace,
    procs: ProcWithMem,
    selectedProc: ProcMemStat | undefined,
    validUpid: number | undefined,
    capture: CaptureInfo | undefined,
    headerMeta: ProcessHeaderMeta | undefined,
    onUpidChange: (upid: number) => void,
  ): m.Children {
    const procName = selectedProc?.procName ?? capture?.processName ?? '…';
    const pid = selectedProc?.pid ?? capture?.pid;
    const durationS = Number(trace.traceInfo.end - trace.traceInfo.start) / 1e9;
    const oomBucket =
      headerMeta?.oomScoreAdj !== undefined
        ? oomScoreBucket(headerMeta.oomScoreAdj)
        : undefined;

    return m('header.pf-memtl-topbar', [
      m('.pf-memtl-topbar__left', [
        m('.pf-memtl-brand', [
          m('.pf-memtl-brand__icon', m(Icon, {icon: 'memory'})),
          m('span.pf-memtl-brand__title', 'Memory'),
          m('span.pf-memtl-brand__badge', 'preview'),
        ]),
        m('.pf-memtl-topbar__sep'),
        m(
          PopupMenu,
          {
            position: PopupPosition.BottomStart,
            trigger: m(
              'button.pf-memtl-proc-btn',
              {type: 'button', title: `${procName} (PID ${pid ?? '?'})`},
              [
                m('span.pf-memtl-proc-btn__name', procName),
                pid !== undefined &&
                  m('span.pf-memtl-proc-btn__pid', `PID ${pid}`),
                m(Icon, {
                  icon: 'arrow_drop_down',
                  className: 'pf-memtl-proc-btn__caret',
                }),
              ],
            ),
          },
          procs.map((p) =>
            m(MenuItem, {
              label: renderProcMenuLabel(p),
              active: p.upid === validUpid,
              onclick: () => onUpidChange(p.upid),
            }),
          ),
        ),
        headerMeta?.uptimeNs !== undefined &&
          m(
            'span.pf-memtl-topbar__meta',
            {title: 'Process uptime at heap dump'},
            [
              m(Icon, {icon: 'schedule'}),
              `Uptime ${formatUptimeShort(headerMeta.uptimeNs)}`,
            ],
          ),
        headerMeta?.oomScoreAdj !== undefined &&
          oomBucket !== undefined &&
          m(
            'span.pf-memtl-topbar__meta',
            {title: `OOM adjustment score: ${headerMeta.oomScoreAdj}`},
            [
              m(
                ColorChip,
                {color: oomBucket.color},
                `${oomBucketLabel(oomBucket)} (${headerMeta.oomScoreAdj})`,
              ),
            ],
          ),
        headerMeta?.dmabufRssSize !== undefined &&
          headerMeta.dmabufRssSize > 0 &&
          m('span.pf-memtl-topbar__meta', {title: 'DMA-BUF RSS at heap dump'}, [
            m(Icon, {icon: 'developer_board'}),
            `DMA-BUF ${formatBytes(headerMeta.dmabufRssSize)}`,
          ]),
        m(
          'span.pf-memtl-topbar__trace',
          `${trace.traceInfo.traceTitle} · ${durationS.toFixed(1)}s`,
        ),
      ]),
      m('.pf-memtl-topbar__right', [
        m(Button, {
          label: 'Timeline',
          icon: 'timeline',
          variant: ButtonVariant.Minimal,
          compact: true,
          onclick: () => trace.navigate('#!/viewer'),
        }),
        m(
          Anchor,
          {
            href: trace.policy.bugReportUrl,
            target: '_blank',
            icon: Icons.ExternalLink,
            className: 'pf-memtl-topbar__feedback',
          },
          'Feedback',
        ),
      ]),
    ]);
  }

  private renderOomeBanner(
    oome: OomeDetails,
    dumpTs: time | undefined,
    onOpenJavaHeap:
      | ((ts: time, view: NavView, params?: Record<string, unknown>) => void)
      | undefined,
  ): m.Children {
    const grid = renderOomeDetailsGrid(oome);
    if (grid === undefined) return undefined;
    return m(
      Panel,
      {className: 'pf-memtl-oome-panel'},
      m(Panel.Header, {
        title: OOME_DETAILS_TITLE,
        subtitle: 'Heap dump triggered by an OutOfMemoryError',
        controls:
          dumpTs !== undefined &&
          onOpenJavaHeap !== undefined &&
          m(Button, {
            label: 'Inspect OOME Callstack',
            icon: 'bug_report',
            variant: ButtonVariant.Outlined,
            intent: Intent.Danger,
            compact: true,
            onclick: () => onOpenJavaHeap(dumpTs, 'callstack'),
          }),
      }),
      m(Panel.Body, grid),
    );
  }

  private renderDefaultChangedHint(
    trace: Trace,
    autoNavigated: boolean,
    openByDefault: Setting<boolean>,
    hideDefaultChangedHint: Setting<boolean>,
  ): m.Children {
    if (
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
          'Memory is now the default page for traces with memory snapshots ' +
            'or heap dumps. Drag across the timeline below to inspect any ' +
            'time range as a delta.',
        ),
        m(
          '.pf-memscope-default-page-callout__actions',
          m(
            ButtonGroup,
            {className: 'pf-memscope-default-page-callout__split-button'},
            m(Button, {
              label: 'Back to Timeline',
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
                label: 'Never open Memory by default',
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

function renderProcMenuLabel(p: ProcMemStat): m.Children {
  const tags: m.Children[] = [];
  if (p.smapsSnapshots > 0) {
    tags.push(
      m('span.pf-memtl-proc-opt__tag', [
        m('span.pf-memscope-capture__dot.pf-memscope-capture__dot--smaps'),
        `${p.smapsSnapshots} smaps`,
      ]),
    );
  }
  if (p.heapDumps > 0) {
    tags.push(
      m('span.pf-memtl-proc-opt__tag', [
        m('span.pf-memscope-capture__dot.pf-memscope-capture__dot--dumps'),
        `${p.heapDumps} java`,
      ]),
    );
  }
  if (p.nativeDumps > 0) {
    tags.push(
      m('span.pf-memtl-proc-opt__tag', [
        m('span.pf-memscope-capture__dot.pf-memscope-capture__dot--native'),
        `${p.nativeDumps} native`,
      ]),
    );
  }
  return m('.pf-memtl-proc-opt', [
    m('.pf-memtl-proc-opt__main', [
      m('span.pf-memtl-proc-opt__name', p.procName),
      m('span.pf-memtl-proc-opt__pid', `PID ${p.pid}`),
    ]),
    tags.length > 0 && m('.pf-memtl-proc-opt__tags', tags),
  ]);
}
