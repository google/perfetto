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

// Hierarchical Left Scope Navigator Rail for the unified Memory Workbench.
// Replaces separate page tabs and nested tab bars with one scannable tree:
//   Process card (with live footprint mini-bar)
//   ├── PROCESS SCOPE (Overview, Memory Map / Smaps, Native Allocations)
//   ├── JAVA HEAP SCOPE (Dump switcher + all 9 HDE analysis lenses)
//   └── OPEN INSPECTORS (Closable flamegraph object sets & object instances)

import m from 'mithril';
import {classNames} from '../../../../base/classnames';
import {Icons} from '../../../../base/semantic_icons';
import {Time, type time} from '../../../../base/time';
import {formatDuration} from '../../../../components/time_utils';
import type {Trace} from '../../../../public/trace';
import {Anchor} from '../../../../widgets/anchor';
import {Button, ButtonVariant} from '../../../../widgets/button';
import {Icon} from '../../../../widgets/icon';
import {MenuItem, PopupMenu} from '../../../../widgets/menu';
import {PopupPosition} from '../../../../widgets/popup';
import type {NavView} from '../../../com.android.HeapDumpExplorer/nav_state';
import type {HeapDumpExplorerSession} from '../../../com.android.HeapDumpExplorer/session';
import type {HeapDump} from '../../../com.android.HeapDumpExplorer/types';
import {formatBytes} from './mem_format';
import type {CaptureInfo, MemoryWorkbenchSection} from './proc_mem_overview';
import type {ProcMemStat, ProcWithMem} from './proc_mem_stats';
import './scope_rail.scss';

export interface HeapLensItem {
  readonly view: Exclude<NavView, 'object' | 'flamegraph-objects'>;
  readonly label: string;
  readonly icon: string;
}

export const HEAP_LENSES: ReadonlyArray<HeapLensItem> = [
  {view: 'overview', label: 'Heap Summary', icon: 'summarize'},
  {view: 'flamegraph', label: 'Flamegraph', icon: 'local_fire_department'},
  {view: 'classes', label: 'Classes', icon: 'category'},
  {view: 'objects', label: 'Objects', icon: 'data_object'},
  {view: 'dominators', label: 'Dominators', icon: 'account_tree'},
  {view: 'bitmaps', label: 'Bitmaps', icon: 'photo_library'},
  {view: 'strings', label: 'Strings', icon: 'text_fields'},
  {view: 'arrays', label: 'Arrays', icon: 'data_array'},
  {view: 'callstack', label: 'Callstack', icon: 'format_list_numbered'},
];

const MAX_INLINE_DUMP_PILLS = 3;

export interface ScopeRailAttrs {
  readonly trace: Trace;
  readonly procs: ProcWithMem;
  readonly selectedProc?: ProcMemStat;
  readonly validUpid?: number;
  readonly capture?: CaptureInfo;
  readonly section: MemoryWorkbenchSection;
  readonly procDumps: ReadonlyArray<HeapDump>;
  readonly hdeSession?: HeapDumpExplorerSession;
  readonly onUpidChange: (upid: number) => void;
  readonly onSelectSection: (section: MemoryWorkbenchSection) => void;
  readonly onSelectHeapView: (view: NavView) => void;
  readonly onSelectDump: (dump: HeapDump) => void;
}

function formatSec(trace: Trace, ts: time): string {
  return `${(Number(Time.diff(ts, trace.traceInfo.start)) / 1e9).toFixed(1)}s`;
}

export class ScopeRail implements m.ClassComponent<ScopeRailAttrs> {
  view({attrs}: m.Vnode<ScopeRailAttrs>): m.Children {
    const {
      trace,
      procs,
      selectedProc,
      validUpid,
      capture,
      section,
      procDumps,
      hdeSession,
      onUpidChange,
      onSelectSection,
      onSelectHeapView,
      onSelectDump,
    } = attrs;

    const procName = selectedProc?.procName ?? capture?.processName ?? '…';
    const pid = selectedProc?.pid ?? capture?.pid;

    const smapsCount = capture?.smaps.samples ?? selectedProc?.smapsSnapshots;
    const nativeCount = capture?.native.samples ?? selectedProc?.nativeDumps;
    const dumpsCount =
      procDumps.length > 0
        ? procDumps.length
        : (capture?.dumps ?? selectedProc?.heapDumps);

    const smapsAvailable = smapsCount === undefined || smapsCount > 0;
    const nativeAvailable = nativeCount === undefined || nativeCount > 0;
    const heapAvailable = dumpsCount === undefined || dumpsCount > 0;

    const activeHdeView =
      section === 'heap' && hdeSession !== undefined
        ? hdeSession.nav.view
        : undefined;
    const activeFg =
      section === 'heap' && hdeSession !== undefined
        ? hdeSession.activeFlamegraph
        : null;
    const activeObjId =
      section === 'heap' && hdeSession !== undefined
        ? hdeSession.activeInstanceObjId
        : null;

    const fgTabs = hdeSession?.flamegraphTabs ?? [];
    const objTabs = hdeSession?.instanceTabs ?? [];
    const hasInspectors = fgTabs.length > 0 || objTabs.length > 0;
    const otherWithHeap = !heapAvailable
      ? procs.find((p) => p.heapDumps > 0)
      : undefined;

    return m('aside.pf-memwb-rail', [
      // Subject Process Card with Live Footprint Mini-Bar
      m('.pf-memwb-rail__top', [
        m('.pf-memwb-rail__brand', [
          m('.pf-memwb-rail__brand-mark', m(Icon, {icon: 'memory'})),
          m('span.pf-memwb-rail__brand-title', 'Memory'),
          m('span.pf-memwb-rail__brand-badge', 'workbench'),
        ]),
        m(
          PopupMenu,
          {
            position: PopupPosition.BottomStart,
            trigger: m(
              'button.pf-memwb-proc-card',
              {type: 'button', title: `${procName} (PID ${pid ?? '?'})`},
              [
                m('.pf-memwb-proc-card__row', [
                  m('span.pf-memwb-proc-card__name', procName),
                  m(Icon, {
                    icon: 'unfold_more',
                    className: 'pf-memwb-proc-card__caret',
                  }),
                ]),
                m('.pf-memwb-proc-card__sub', [
                  pid !== undefined &&
                    m('span.pf-memwb-proc-card__pid', `PID ${pid}`),
                  capture !== undefined &&
                    capture.footprintTotal > 0 &&
                    m(
                      'span.pf-memwb-proc-card__total',
                      `${formatBytes(capture.footprintTotal)} RSS+Swap`,
                    ),
                ]),
                this.renderFootprintBar(capture),
              ],
            ),
          },
          procs.map((p) =>
            m(MenuItem, {
              label: renderProcMenuOption(p),
              active: p.upid === validUpid,
              onclick: () => onUpidChange(p.upid),
            }),
          ),
        ),
      ]),

      // Scrollable Hierarchical Scope Tree
      m('nav.pf-memwb-tree', {'aria-label': 'Memory scopes'}, [
        // 1. PROCESS SCOPE
        m('.pf-memwb-group', [
          m('.pf-memwb-group__header', [
            m('span.pf-memwb-group__title', 'Process'),
            m('span.pf-memwb-group__hint', 'trace-wide'),
          ]),
          this.renderNavItem({
            icon: 'dashboard',
            label: 'Overview',
            active: section === 'overview',
            onclick: () => onSelectSection('overview'),
          }),
          this.renderNavItem({
            icon: 'table_rows',
            label: 'Memory Map (Smaps)',
            active: section === 'smaps',
            disabled: !smapsAvailable,
            dot: smapsAvailable ? 'smaps' : undefined,
            badge:
              smapsCount !== undefined && smapsCount > 0
                ? `${smapsCount}`
                : undefined,
            tooltip: !smapsAvailable
              ? 'No smaps snapshots captured for this process'
              : 'Virtual memory mappings (RSS, PSS, swap, private/shared)',
            onclick: () => onSelectSection('smaps'),
          }),
          this.renderNavItem({
            icon: 'memory_alt',
            label: 'Native Allocations',
            active: section === 'native',
            disabled: !nativeAvailable,
            dot: nativeAvailable ? 'native' : undefined,
            badge:
              nativeCount !== undefined && nativeCount > 0
                ? `${nativeCount}`
                : undefined,
            tooltip: !nativeAvailable
              ? 'No heapprofd native profile captured for this process'
              : 'Unreleased native allocation callstacks (heapprofd)',
            onclick: () => onSelectSection('native'),
          }),
        ]),

        // 2. JAVA HEAP SCOPE
        m('.pf-memwb-group', [
          m('.pf-memwb-group__header', [
            m('span.pf-memwb-group__title', [
              m('span.pf-memscope-capture__dot.pf-memscope-capture__dot--dump'),
              'Java Heap',
            ]),
            dumpsCount !== undefined &&
              dumpsCount > 0 &&
              m(
                'span.pf-memwb-group__hint',
                `${dumpsCount} ${dumpsCount === 1 ? 'dump' : 'dumps'}`,
              ),
          ]),
          heapAvailable && hdeSession !== undefined && procDumps.length > 0
            ? [
                this.renderRailDumpSwitcher(
                  trace,
                  hdeSession,
                  procDumps,
                  onSelectDump,
                ),
                m(
                  '.pf-memwb-heap-branch',
                  HEAP_LENSES.map((lens) =>
                    this.renderNavItem({
                      icon: lens.icon,
                      label: lens.label,
                      active: activeHdeView === lens.view,
                      badge:
                        lens.view === 'bitmaps' && capture?.hasBitmaps
                          ? '●'
                          : undefined,
                      onclick: () => onSelectHeapView(lens.view),
                    }),
                  ),
                ),
              ]
            : m('.pf-memwb-group__empty', [
                m('span', 'No Java heap dump for this process.'),
                otherWithHeap !== undefined &&
                  m(Button, {
                    label: `Switch to ${otherWithHeap.procName}`,
                    icon: 'account_tree',
                    variant: ButtonVariant.Outlined,
                    compact: true,
                    onclick: () => onUpidChange(otherWithHeap.upid),
                  }),
              ]),
        ]),

        // 3. OPEN INSPECTORS (Dynamic drill-downs — no horizontal tab overflow!)
        hasInspectors &&
          hdeSession !== undefined &&
          m('.pf-memwb-group', [
            m('.pf-memwb-group__header', [
              m('span.pf-memwb-group__title', 'Inspecting'),
              m(
                'span.pf-memwb-group__hint',
                `${fgTabs.length + objTabs.length}`,
              ),
            ]),
            fgTabs.map((fg) => {
              const isAct =
                activeFg !== null &&
                activeFg.pathHashes === fg.pathHashes &&
                activeFg.isDominator === fg.isDominator;
              const title =
                fg.count !== null
                  ? `Flamegraph (${fg.count.toLocaleString()})`
                  : 'Flamegraph objects';
              return this.renderNavItem({
                icon: 'filter_list',
                label: title,
                active: isAct,
                onclick: () =>
                  hdeSession.navigate('flamegraph-objects', {
                    pathHashes: fg.pathHashes,
                    isDominator: fg.isDominator,
                  }),
                onClose: () =>
                  hdeSession.closeFlamegraph(fg.pathHashes, fg.isDominator),
              });
            }),
            objTabs.map((obj) => {
              const isAct = activeObjId === obj.objId;
              return this.renderNavItem({
                icon: 'deployed_code',
                label: obj.label,
                active: isAct,
                onclick: () => hdeSession.navigate('object', {id: obj.objId}),
                onClose: () => hdeSession.closeInstanceTab(obj.objId),
              });
            }),
          ]),
      ]),

      // Rail Footer
      m('.pf-memwb-rail__footer', [
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
            className: 'pf-memwb-rail__feedback',
          },
          'Feedback',
        ),
      ]),
    ]);
  }

  private renderFootprintBar(capture?: CaptureInfo): m.Children {
    if (
      capture === undefined ||
      capture.footprintTotal <= 0 ||
      capture.footprintSegments.length === 0
    ) {
      return null;
    }

    const total = capture.footprintTotal;
    const width = 200;
    let cursor = 0;
    const rects: m.Children[] = [];
    for (const seg of capture.footprintSegments) {
      if (seg.bytes <= 0) continue;
      const w = (seg.bytes / total) * width;
      rects.push(
        m(
          'rect',
          {
            key: seg.key,
            x: cursor,
            y: 0,
            width: Math.max(1, w),
            height: 6,
            fill: seg.color,
          },
          m('title', `${seg.label}: ${formatBytes(seg.bytes)}`),
        ),
      );
      cursor += w;
    }

    return m(
      'svg.pf-memwb-footprint-bar',
      {
        'viewBox': `0 0 ${width} 6`,
        'preserveAspectRatio': 'none',
        'aria-hidden': 'true',
      },
      rects,
    );
  }

  private renderRailDumpSwitcher(
    trace: Trace,
    hdeSession: HeapDumpExplorerSession,
    procDumps: ReadonlyArray<HeapDump>,
    onSelectDump: (dump: HeapDump) => void,
  ): m.Children {
    const active = hdeSession.activeDump;
    const activeIdx = active !== null ? procDumps.indexOf(active) : -1;

    if (procDumps.length === 1) {
      const only = procDumps[0];
      if (only === undefined) return null;
      return m('.pf-memwb-dump-card', [
        m(Icon, {icon: 'photo_camera', className: 'pf-memwb-dump-card__icon'}),
        m(
          'span.pf-memwb-dump-card__label',
          `Dump #1 · t=${formatSec(trace, only.ts)}`,
        ),
      ]);
    }

    if (procDumps.length <= MAX_INLINE_DUMP_PILLS) {
      return m('.pf-memwb-dump-pills', [
        procDumps.map((d, i) => {
          const isAct = d === active;
          return m(
            'button.pf-memwb-dump-pill',
            {
              key: `${d.upid}:${d.ts.toString()}`,
              type: 'button',
              className: classNames(isAct && 'pf-memwb-dump-pill--active'),
              title: `Heap dump #${i + 1} at ${formatDuration(trace, Time.diff(d.ts, trace.traceInfo.start))}`,
              onclick: () => onSelectDump(d),
            },
            [
              m('span.pf-memwb-dump-pill__idx', `#${i + 1}`),
              m('span.pf-memwb-dump-pill__ts', formatSec(trace, d.ts)),
            ],
          );
        }),
      ]);
    }

    const triggerLabel =
      active !== null && activeIdx >= 0
        ? `Dump #${activeIdx + 1}/${procDumps.length} · ${formatSec(trace, active.ts)}`
        : `${procDumps.length} dumps`;

    return m(
      '.pf-memwb-dump-dropdown',
      m(
        PopupMenu,
        {
          position: PopupPosition.BottomStart,
          trigger: m(Button, {
            label: triggerLabel,
            icon: 'photo_camera',
            rightIcon: 'arrow_drop_down',
            variant: ButtonVariant.Outlined,
            compact: true,
            className: 'pf-memwb-dump-dropdown__btn',
          }),
        },
        procDumps.map((d, i) => {
          const offset = Time.diff(d.ts, trace.traceInfo.start);
          return m(MenuItem, {
            label: `Dump #${i + 1} — ${formatDuration(trace, offset)}`,
            active: d === active,
            onclick: () => onSelectDump(d),
          });
        }),
      ),
    );
  }

  private renderNavItem(opts: {
    readonly icon: string;
    readonly label: string;
    readonly active: boolean;
    readonly disabled?: boolean;
    readonly dot?: 'smaps' | 'native' | 'dump';
    readonly badge?: string;
    readonly tooltip?: string;
    readonly onclick: () => void;
    readonly onClose?: () => void;
  }): m.Children {
    const {
      icon,
      label,
      active,
      disabled,
      dot,
      badge,
      tooltip,
      onclick,
      onClose,
    } = opts;

    return m(
      '.pf-memwb-nav-item',
      {
        className: classNames(
          active && 'pf-memwb-nav-item--active',
          disabled && 'pf-memwb-nav-item--disabled',
        ),
        title: tooltip ?? label,
      },
      [
        m(
          'button.pf-memwb-nav-item__btn',
          {
            type: 'button',
            disabled,
            onclick: () => {
              if (!disabled) onclick();
            },
          },
          [
            m(Icon, {icon, className: 'pf-memwb-nav-item__icon'}),
            m('span.pf-memwb-nav-item__label', label),
            dot !== undefined &&
              m(`span.pf-memwb-nav-item__dot.pf-memscope-capture__dot--${dot}`),
            badge !== undefined && m('span.pf-memwb-nav-item__badge', badge),
          ],
        ),
        onClose !== undefined &&
          m(
            'button.pf-memwb-nav-item__close',
            {
              type: 'button',
              title: `Close ${label}`,
              onclick: (e: MouseEvent) => {
                e.stopPropagation();
                onClose();
              },
            },
            m(Icon, {icon: 'close'}),
          ),
      ],
    );
  }
}

function renderProcMenuOption(p: ProcMemStat): m.Children {
  const tags: m.Children[] = [];
  if (p.smapsSnapshots > 0) {
    tags.push(
      m('span.pf-memwb-proc-option__tag', [
        m('span.pf-memscope-capture__dot.pf-memscope-capture__dot--smaps'),
        `${p.smapsSnapshots} smaps`,
      ]),
    );
  }
  if (p.heapDumps > 0) {
    tags.push(
      m('span.pf-memwb-proc-option__tag', [
        m('span.pf-memscope-capture__dot.pf-memscope-capture__dot--dump'),
        `${p.heapDumps} java`,
      ]),
    );
  }
  if (p.nativeDumps > 0) {
    tags.push(
      m('span.pf-memwb-proc-option__tag', [
        m('span.pf-memscope-capture__dot.pf-memscope-capture__dot--native'),
        `${p.nativeDumps} native`,
      ]),
    );
  }
  return m('.pf-memwb-proc-option', [
    m('.pf-memwb-proc-option__main', [
      m('span.pf-memwb-proc-option__name', p.procName),
      m('span.pf-memwb-proc-option__pid', `PID ${p.pid}`),
    ]),
    tags.length > 0 && m('.pf-memwb-proc-option__tags', tags),
  ]);
}
