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
import {Anchor} from '../../../../widgets/anchor';
import {Button, ButtonGroup, ButtonVariant} from '../../../../widgets/button';
import {Chip} from '../../../../widgets/chip';
import {Intent} from '../../../../widgets/common';
import {EmptyState} from '../../../../widgets/empty_state';
import {Icon} from '../../../../widgets/icon';
import {MenuItem, PopupMenu} from '../../../../widgets/menu';
import {PopupPosition} from '../../../../widgets/popup';
import {HeapDumpPage} from '../../../com.android.HeapDumpExplorer/heap_dump_page';
import type {NavView} from '../../../com.android.HeapDumpExplorer/nav_state';
import type {HeapDumpExplorerSession} from '../../../com.android.HeapDumpExplorer/session';
import type {HeapDump} from '../../../com.android.HeapDumpExplorer/types';
import {Callout} from '../../components/callout';
import {Panel} from '../../components/panel';
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
import {NativeSection} from './summary/native_section';
import {TraceOverview} from './summary/trace_overview';
import './memory_overview_page.scss';

export interface MemoryOverviewPageAttrs {
  readonly trace: Trace;
  readonly upid?: number;
  readonly hdeSession?: HeapDumpExplorerSession;
  readonly hdeSubpage?: string;
  readonly focusHeapExplorer?: boolean;
  readonly autoNavigated: boolean;
  readonly openByDefault: Setting<boolean>;
  readonly hideDefaultChangedHint: Setting<boolean>;
  readonly onUpidChange: (upid: number) => void;
  readonly onOpenJavaHeap: (
    ts: time,
    view: NavView,
    params?: Record<string, unknown>,
  ) => void;
}

const MAX_INLINE_DUMP_CHIPS = 4;

const SECTION_IDS = {
  timeline: 'pf-memtl-sec-timeline',
  breakdown: 'pf-memtl-sec-breakdown',
  java: 'pf-memtl-sec-java',
  bitmaps: 'pf-memtl-sec-bitmaps',
  native: 'pf-memtl-sec-native',
  smaps: 'pf-memtl-sec-smaps',
  heap: 'pf-memtl-sec-heap',
} as const;

function scrollToSection(id: string): void {
  const el = document.getElementById(id);
  el?.scrollIntoView({behavior: 'smooth', block: 'start'});
}

function scrollToSectionIfNeeded(id: string): void {
  const el = document.getElementById(id);
  if (!el) return;
  const rect = el.getBoundingClientRect();
  if (rect.top < 0 || rect.top > window.innerHeight * 0.65) {
    el.scrollIntoView({behavior: 'smooth', block: 'start'});
  }
}

function formatOffsetSec(trace: Trace, ts: time): string {
  return `${(Number(Time.diff(ts, trace.traceInfo.start)) / 1e9).toFixed(1)}s`;
}

export class MemoryOverviewPage implements m.ClassComponent<MemoryOverviewPageAttrs> {
  private readonly procsSlot = new AsyncMemo<ProcWithMem>();
  private readonly captureSlot = new AsyncMemo<CaptureInfo>();
  private readonly selectionByUpid = new Map<number, MemSelection>();
  private heapExpanded = false;
  private lastFocusedSubpage?: string;

  onremove() {
    this.procsSlot.dispose();
    this.captureSlot.dispose();
  }

  onupdate({attrs}: m.VnodeDOM<MemoryOverviewPageAttrs>) {
    if (
      attrs.focusHeapExplorer &&
      attrs.hdeSubpage !== this.lastFocusedSubpage
    ) {
      this.lastFocusedSubpage = attrs.hdeSubpage;
      requestAnimationFrame(() => scrollToSectionIfNeeded(SECTION_IDS.heap));
    }
  }

  oncreate({attrs}: m.VnodeDOM<MemoryOverviewPageAttrs>) {
    if (attrs.focusHeapExplorer) {
      this.lastFocusedSubpage = attrs.hdeSubpage;
      requestAnimationFrame(() => scrollToSectionIfNeeded(SECTION_IDS.heap));
    }
  }

  view({attrs}: m.Vnode<MemoryOverviewPageAttrs>) {
    const {
      trace,
      upid,
      hdeSession,
      hdeSubpage,
      autoNavigated,
      openByDefault,
      hideDefaultChangedHint,
      onUpidChange,
      onOpenJavaHeap,
    } = attrs;

    const procs = this.procsSlot.use({
      key: trace.traceInfo.uuid,
      compute: () => loadProcessMemoryStats(trace.engine),
    }).data;

    if (procs === undefined) {
      return m(
        '.pf-memscope-page.pf-memtl-page',
        m(
          '.pf-memtl-page__empty',
          m(EmptyState, {icon: 'hourglass', title: 'Loading processes...'}),
        ),
      );
    }

    if (procs.length === 0) {
      return m(
        '.pf-memscope-page.pf-memtl-page',
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
    if (validUpid !== undefined) {
      try {
        capture = this.captureSlot.use({
          key: {traceId: trace.traceInfo.uuid, upid: validUpid},
          compute: () => loadCaptureInfo(trace, validUpid),
        }).data;
      } catch (e) {
        captureError = String(e);
      }
    }

    const procDumps =
      validUpid !== undefined && hdeSession !== undefined
        ? hdeSession.getDumpsForProcess(validUpid)
        : [];
    const currentSelection =
      validUpid !== undefined ? this.selectionByUpid.get(validUpid) : undefined;

    const smapsAvailable = capture === undefined || capture.smaps.samples > 0;
    const dumpsAvailable =
      procDumps.length > 0 || capture === undefined || capture.dumps > 0;
    const bitmapsAvailable = capture === undefined || capture.hasBitmaps;
    const nativeAvailable = capture === undefined || capture.native.samples > 0;

    const handleOpenJavaHeap = (
      ts: time,
      view: NavView,
      params?: Record<string, unknown>,
    ) => {
      onOpenJavaHeap(ts, view, params);
      requestAnimationFrame(() => scrollToSection(SECTION_IDS.heap));
    };

    const jumpItems: ReadonlyArray<{
      readonly id: string;
      readonly label: string;
      readonly icon: string;
      readonly available: boolean;
    }> = [
      {
        id: SECTION_IDS.breakdown,
        label: 'Breakdown',
        icon: 'donut_large',
        available: smapsAvailable,
      },
      {
        id: SECTION_IDS.java,
        label: 'Java',
        icon: 'coffee',
        available: dumpsAvailable,
      },
      {
        id: SECTION_IDS.bitmaps,
        label: 'Bitmaps',
        icon: 'photo_library',
        available: bitmapsAvailable,
      },
      {
        id: SECTION_IDS.native,
        label: 'Native',
        icon: 'memory_alt',
        available: nativeAvailable,
      },
      {
        id: SECTION_IDS.smaps,
        label: 'Smaps VMAs',
        icon: 'table_rows',
        available: smapsAvailable,
      },
      {
        id: SECTION_IDS.heap,
        label: 'Heap Explorer',
        icon: 'account_tree',
        available: procDumps.length > 0,
      },
    ];

    // Order detail sections with data ahead of empty sections while preserving
    // natural triage order within each group.
    const flowSections =
      validUpid !== undefined
        ? [
            {
              key: 'breakdown',
              hasData: smapsAvailable,
              content: m(
                'section.pf-memtl-section',
                {id: SECTION_IDS.breakdown, key: 'breakdown'},
                m(MemoryMap, {
                  trace,
                  upid: validUpid,
                  selTs: currentSelection?.sel,
                  baseTs: currentSelection?.base,
                  onInspectSmaps: () => scrollToSection(SECTION_IDS.smaps),
                }),
              ),
            },
            {
              key: 'java',
              hasData: dumpsAvailable,
              content: m(
                'section.pf-memtl-section',
                {id: SECTION_IDS.java, key: 'java'},
                m(JavaSection, {
                  trace,
                  upid: validUpid,
                  selTs: currentSelection?.sel,
                  baseTs: currentSelection?.base,
                  onOpenJavaHeap:
                    procDumps.length > 0 ? handleOpenJavaHeap : undefined,
                }),
              ),
            },
            {
              key: 'bitmaps',
              hasData: bitmapsAvailable,
              content: m(
                'section.pf-memtl-section',
                {id: SECTION_IDS.bitmaps, key: 'bitmaps'},
                m(BitmapsSection, {
                  trace,
                  upid: validUpid,
                  selTs: currentSelection?.sel,
                  baseTs: currentSelection?.base,
                  onOpenBitmapsGallery:
                    procDumps.length > 0
                      ? (ts: time) => handleOpenJavaHeap(ts, 'bitmaps')
                      : undefined,
                }),
              ),
            },
            {
              key: 'native',
              hasData: nativeAvailable,
              content: m(
                'section.pf-memtl-section',
                {id: SECTION_IDS.native, key: 'native'},
                m(NativeSection, {
                  trace,
                  upid: validUpid,
                  selTs: currentSelection?.sel,
                  baseTs: currentSelection?.base,
                }),
              ),
            },
            {
              key: 'smaps',
              hasData: smapsAvailable,
              content: m(
                'section.pf-memtl-section',
                {id: SECTION_IDS.smaps, key: 'smaps'},
                m(SmapsDetail, {
                  trace,
                  upid: validUpid,
                  selTs: currentSelection?.sel,
                  baseTs: currentSelection?.base,
                  hideSnapshotPicker: true,
                }),
              ),
            },
            {
              key: 'heap-explorer',
              hasData: procDumps.length > 0,
              content: this.renderHeapExplorerSection({
                trace,
                procDumps,
                hdeSession,
                hdeSubpage,
                selection: currentSelection,
              }),
            },
          ]
        : [];
    flowSections.sort((a, b) => Number(b.hasData) - Number(a.hasData));

    return m('.pf-memscope-page.pf-memtl-page', [
      m('.pf-memscope-page__content.pf-memtl-page__content', [
        // Compact Top Subject Header
        this.renderTopBar(
          trace,
          procs,
          selectedProc,
          validUpid,
          capture,
          onUpidChange,
        ),

        this.renderDefaultChangedHint(
          trace,
          autoNavigated,
          openByDefault,
          hideDefaultChangedHint,
        ),

        captureError !== undefined && m('p.pf-error', `Error: ${captureError}`),

        validUpid === undefined &&
          m(EmptyState, {
            icon: 'error_outline',
            title: 'Unable to resolve process from URL',
          }),

        validUpid !== undefined &&
          captureError === undefined && [
            // HERO: Interactive Memory Timeline front and center at the top!
            m(
              'section.pf-memtl-section',
              {id: SECTION_IDS.timeline},
              m(MemoryTimelineHero, {
                trace,
                upid: validUpid,
                procDumps,
                activeDump: hdeSession?.activeDump ?? null,
                selection: currentSelection,
                onSelect: (next) => {
                  if (next === undefined) {
                    this.selectionByUpid.delete(validUpid);
                  } else {
                    this.selectionByUpid.set(validUpid, next);
                  }
                },
                onSelectDump: (dump) => {
                  hdeSession?.selectDump(dump);
                },
              }),
            ),

            // Sticky Section Jump & Active Range Bar
            this.renderStickyJumpBar(trace, jumpItems, currentSelection, () =>
              this.selectionByUpid.delete(validUpid),
            ),

            // Headline Process Billboards
            m(TraceOverview, {trace, upid: validUpid}),

            // Flowing Reactive Subsections (own keyed fragment so sections keep
            // state when reordered once capture availability resolves).
            flowSections.map((s) => s.content),
          ],
      ]),
    ]);
  }

  private renderTopBar(
    trace: Trace,
    procs: ProcWithMem,
    selectedProc: ProcMemStat | undefined,
    validUpid: number | undefined,
    capture: CaptureInfo | undefined,
    onUpidChange: (upid: number) => void,
  ): m.Children {
    const procName = selectedProc?.procName ?? capture?.processName ?? '…';
    const pid = selectedProc?.pid ?? capture?.pid;
    const durationS = Number(trace.traceInfo.end - trace.traceInfo.start) / 1e9;

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

  private renderStickyJumpBar(
    trace: Trace,
    items: ReadonlyArray<{
      readonly id: string;
      readonly label: string;
      readonly icon: string;
      readonly available: boolean;
    }>,
    selection: MemSelection | undefined,
    onClearSelection: () => void,
  ): m.Children {
    const isRange = selection?.base !== undefined;

    return m('nav.pf-memtl-jumpbar', {'aria-label': 'Jump to section'}, [
      m('.pf-memtl-jumpbar__links', [
        m(
          'button.pf-memtl-jumpbar__btn.pf-memtl-jumpbar__btn--top',
          {
            type: 'button',
            title: 'Back to Memory Timeline',
            onclick: () => scrollToSection(SECTION_IDS.timeline),
          },
          [m(Icon, {icon: 'arrow_upward'}), 'Timeline'],
        ),
        items.map((item) =>
          m(
            'button.pf-memtl-jumpbar__btn',
            {
              key: item.id,
              type: 'button',
              disabled: !item.available,
              className: classNames(
                !item.available && 'pf-memtl-jumpbar__btn--muted',
              ),
              onclick: () => scrollToSection(item.id),
            },
            [m(Icon, {icon: item.icon}), item.label],
          ),
        ),
      ]),
      m('.pf-memtl-jumpbar__context', [
        isRange && selection?.base !== undefined
          ? m('.pf-memtl-jumpbar__pill.pf-memtl-jumpbar__pill--delta', [
              m(Icon, {icon: 'compare_arrows'}),
              m(
                'span',
                `Δ ${formatOffsetSec(trace, selection.base)} → ${formatOffsetSec(trace, selection.sel)}`,
              ),
              m(Button, {
                icon: 'close',
                compact: true,
                title: 'Clear time range',
                onclick: onClearSelection,
              }),
            ])
          : selection !== undefined
            ? m('.pf-memtl-jumpbar__pill', [
                m(Icon, {icon: 'photo_camera'}),
                m('span', `Pinned @ ${formatOffsetSec(trace, selection.sel)}`),
                m(Button, {
                  icon: 'close',
                  compact: true,
                  title: 'Reset to latest',
                  onclick: onClearSelection,
                }),
              ])
            : m(
                'span.pf-memtl-jumpbar__hint',
                'Drag timeline above to show deltas (Δ)',
              ),
      ]),
    ]);
  }

  private renderHeapExplorerSection(opts: {
    readonly trace: Trace;
    readonly procDumps: ReadonlyArray<HeapDump>;
    readonly hdeSession?: HeapDumpExplorerSession;
    readonly hdeSubpage?: string;
    readonly selection?: MemSelection;
  }): m.Child {
    const {trace, procDumps, hdeSession, hdeSubpage, selection} = opts;

    if (hdeSession === undefined || procDumps.length === 0) {
      return m(
        'section.pf-memtl-section',
        {id: SECTION_IDS.heap, key: 'heap-explorer'},
        m(
          Panel,
          {className: 'pf-memscope-panel--empty'},
          m(Panel.Header, {title: 'Java Heap Explorer'}),
          m(
            Panel.Body,
            m(
              '.pf-memscope-landing__empty',
              m(
                EmptyState,
                {
                  icon: 'account_tree',
                  title: 'No Java heap dumps for this process',
                },
                'Capture a java_hprof dump to inspect objects, flamegraph, ' +
                  'dominators, bitmaps, and strings.',
              ),
            ),
          ),
        ),
      );
    }

    const activeDump = hdeSession.activeDump;
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

    const controls: m.Children[] = [];
    if (procDumps.length === 1) {
      const only = procDumps[0];
      if (only !== undefined) {
        controls.push(
          m(
            'span.pf-memtl-heap-card__dump-tag',
            `Dump #1 · t=${formatOffsetSec(trace, only.ts)}`,
          ),
        );
      }
    } else if (procDumps.length <= MAX_INLINE_DUMP_CHIPS) {
      controls.push(
        m(
          '.pf-memtl-heap-card__dump-chips',
          procDumps.map((d, i) =>
            m(Chip, {
              key: `${d.upid}:${d.ts.toString()}`,
              label: `#${i + 1} · ${formatOffsetSec(trace, d.ts)}`,
              rounded: true,
              interactive: true,
              intent: d === activeDump ? Intent.Primary : Intent.None,
              onclick: () => hdeSession.selectDump(d),
            }),
          ),
        ),
      );
    } else {
      const activeIdx =
        activeDump !== null ? procDumps.indexOf(activeDump) : -1;
      controls.push(
        m(
          PopupMenu,
          {
            position: PopupPosition.BottomEnd,
            trigger: m(Button, {
              label:
                activeDump !== null && activeIdx >= 0
                  ? `Dump #${activeIdx + 1}/${procDumps.length} · ${formatOffsetSec(trace, activeDump.ts)}`
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
              onclick: () => hdeSession.selectDump(d),
            }),
          ),
        ),
      );
    }

    controls.push(
      m(Button, {
        label: this.heapExpanded ? 'Exit fullscreen' : 'Expand',
        icon: this.heapExpanded ? 'close_fullscreen' : 'open_in_full',
        variant: ButtonVariant.Outlined,
        compact: true,
        onclick: () => {
          this.heapExpanded = !this.heapExpanded;
        },
      }),
    );

    const subtitle = rangeSpansTwoDumps
      ? `Inspecting target Dump #${procDumps.indexOf(selDump) + 1} ` +
        `(t=${formatOffsetSec(trace, selDump.ts)}); see Java Memory & ` +
        `Bitmaps sections above for Δ vs Dump #${procDumps.indexOf(baseDump) + 1}.`
      : 'Object-level inspection: flamegraph, classes, dominators, bitmaps, ' +
        'strings, arrays, and individual instances.';

    return m(
      'section.pf-memtl-section',
      {id: SECTION_IDS.heap, key: 'heap-explorer'},
      m(
        Panel,
        {
          className: classNames(
            'pf-memtl-heap-card',
            this.heapExpanded && 'pf-memtl-heap-card--expanded',
          ),
        },
        m(Panel.Header, {
          title: 'Java Heap Explorer',
          subtitle,
          controls,
        }),
        m(
          '.pf-memtl-heap-card__body',
          m(HeapDumpPage, {
            session: hdeSession,
            subpage: hdeSubpage,
            embedded: true,
            active: true,
          }),
        ),
      ),
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
        m('span.pf-memscope-capture__dot.pf-memscope-capture__dot--dump'),
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
