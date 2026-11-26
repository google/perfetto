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
import {assertIsInstance} from '../../../../base/assert';
import {classNames} from '../../../../base/classnames';
import {Icons} from '../../../../base/semantic_icons';
import {Time} from '../../../../base/time';
import {formatDuration} from '../../../../components/time_utils';
import type {Trace} from '../../../../public/trace';
import {Button} from '../../../../widgets/button';
import {Icon} from '../../../../widgets/icon';
import {Select} from '../../../../widgets/select';
import type {NavView} from '../../../com.android.HeapDumpExplorer/nav_state';
import type {HeapDumpExplorerSession} from '../../../com.android.HeapDumpExplorer/session';
import type {HeapDump} from '../../../com.android.HeapDumpExplorer/types';
import type {MemoryTab} from './memory_route';
import type {ProcMemStat, ProcWithMem} from './proc_mem_stats';
import './memory_rail.scss';

type StaticHeapView = Exclude<NavView, 'object' | 'flamegraph-objects'>;

interface HeapNavEntry {
  readonly view: StaticHeapView;
  readonly label: string;
  readonly icon: string;
}

const HEAP_NAV_ENTRIES: ReadonlyArray<HeapNavEntry> = [
  {view: 'overview', label: 'Summary', icon: 'summarize'},
  {view: 'flamegraph', label: 'Flamegraph', icon: 'bar_chart'},
  {view: 'classes', label: 'Classes', icon: 'category'},
  {view: 'objects', label: 'Objects', icon: 'data_object'},
  {view: 'dominators', label: 'Dominators', icon: 'account_tree'},
  {view: 'bitmaps', label: 'Bitmaps', icon: 'image'},
  {view: 'strings', label: 'Strings', icon: 'text_fields'},
  {view: 'arrays', label: 'Arrays', icon: 'data_array'},
  {view: 'callstack', label: 'Callstack', icon: 'layers'},
];

export interface MemoryRailAttrs {
  readonly trace: Trace;
  readonly procs: ProcWithMem;
  readonly selectedUpid: number | undefined;
  readonly selectedProc: ProcMemStat | undefined;
  readonly activeTab: MemoryTab;
  readonly hdeSession: HeapDumpExplorerSession;
  readonly onUpidChange: (upid: number) => void;
  readonly onSelectProcessTab: (tab: 'summary' | 'smaps') => void;
  readonly onSelectHeapView: (view: StaticHeapView) => void;
  readonly onSelectHeapDump: (dump: HeapDump) => void;
}

// Collapsible left navigation rail for the unified Memory workbench. Organizes
// process-level views (Memscope Overview & Smaps) and dump-level views (Heap
// Dump Explorer views + open dynamic inspectors) in a single vertical scope
// hierarchy, eliminating stacked horizontal tab bars.
export class MemoryRail implements m.ClassComponent<MemoryRailAttrs> {
  private collapsed = false;

  view({attrs}: m.Vnode<MemoryRailAttrs>): m.Children {
    const {
      trace,
      procs,
      selectedUpid,
      selectedProc,
      activeTab,
      hdeSession,
      onUpidChange,
      onSelectProcessTab,
      onSelectHeapView,
      onSelectHeapDump,
    } = attrs;

    const procDumps =
      selectedUpid !== undefined ? hdeSession.dumpsForUpid(selectedUpid) : [];
    const activeDump =
      hdeSession.activeDump?.upid === selectedUpid
        ? hdeSession.activeDump
        : (procDumps[0] ?? null);
    const showInspectors =
      selectedUpid !== undefined &&
      hdeSession.activeDump?.upid === selectedUpid;
    const fgTabs = showInspectors ? hdeSession.flamegraphTabs : [];
    const instTabs = showInspectors ? hdeSession.instanceTabs : [];
    const inspectorCount = fgTabs.length + instTabs.length;

    if (this.collapsed) {
      return this.renderCollapsedRail(
        trace,
        selectedProc,
        activeTab,
        activeDump,
        hdeSession,
        inspectorCount,
        onSelectProcessTab,
        onSelectHeapView,
      );
    }

    const smapsCount = selectedProc?.smapsSnapshots ?? 0;
    const hprofCount = selectedProc?.heapDumps ?? 0;
    const nativeCount = selectedProc?.nativeDumps ?? 0;
    const inHeap = activeTab === 'heap';
    const activeHeapView = inHeap ? hdeSession.nav.view : undefined;
    const activeFg = inHeap ? hdeSession.activeFlamegraph : null;
    const activeObjId = inHeap ? hdeSession.activeInstanceObjId : null;

    return m('aside.pf-memscope-rail', [
      // Top scope header: process picker + per-source presence indicators.
      m('.pf-memscope-rail__process-card', [
        m('.pf-memscope-rail__process-top', [
          m('span.pf-memscope-rail__section-title', 'Process'),
          m(Button, {
            icon: Icons.GoBack,
            compact: true,
            title: 'Collapse navigation rail',
            onclick: () => {
              this.collapsed = true;
            },
          }),
        ]),
        procs.length > 1
          ? m(
              Select,
              {
                className: 'pf-memscope-rail__select',
                value: selectedUpid?.toString(),
                title:
                  selectedProc !== undefined
                    ? `${selectedProc.procName} (pid ${selectedProc.pid})`
                    : undefined,
                onchange: (e: Event) => {
                  assertIsInstance(e.target, HTMLSelectElement);
                  onUpidChange(Number(e.target.value));
                },
              },
              procs.map((p) =>
                m('option', {value: p.upid.toString()}, procOptionLabel(p)),
              ),
            )
          : selectedProc !== undefined &&
            m(
              '.pf-memscope-rail__single-proc',
              {title: `${selectedProc.procName} (pid ${selectedProc.pid})`},
              [
                m(
                  'span.pf-memscope-rail__single-proc-name',
                  selectedProc.procName,
                ),
                m(
                  'span.pf-memscope-rail__single-proc-pid',
                  `pid ${selectedProc.pid}`,
                ),
              ],
            ),
        selectedProc !== undefined &&
          m('.pf-memscope-rail__sources', [
            sourcePill('smaps', smapsCount, 'smaps'),
            sourcePill('dump', hprofCount, 'hprof'),
            sourcePill('native', nativeCount, 'heapprofd'),
          ]),
      ]),

      // Scrollable navigation sections.
      m('.pf-memscope-rail__nav', [
        // Section 1: Process-wide memory views (Memscope).
        m('.pf-memscope-rail__section', [
          m(
            '.pf-memscope-rail__section-header',
            m('span.pf-memscope-rail__section-title', 'Process Memory'),
          ),
          railItem({
            icon: 'dashboard',
            label: 'Overview',
            active: activeTab === 'summary',
            onclick: () => onSelectProcessTab('summary'),
          }),
          railItem({
            icon: 'table_rows',
            label: 'Smaps',
            badge: smapsCount > 0 ? String(smapsCount) : undefined,
            active: activeTab === 'smaps',
            disabled: smapsCount === 0,
            title:
              smapsCount === 0
                ? 'No smaps snapshots available for this process'
                : undefined,
            onclick: () => onSelectProcessTab('smaps'),
          }),
        ]),

        // Section 2: Java Heap dump views (Heap Dump Explorer).
        m('.pf-memscope-rail__section', [
          m('.pf-memscope-rail__section-header', [
            m('span.pf-memscope-rail__section-title', 'Java Heap'),
            activeDump !== null &&
              m(
                'span.pf-memscope-rail__section-meta',
                procDumps.length > 1
                  ? `${procDumps.length} dumps`
                  : `@ ${formatDuration(trace, Time.diff(activeDump.ts, trace.traceInfo.start))}`,
              ),
          ]),
          procDumps.length > 1 &&
            activeDump !== null &&
            m(
              '.pf-memscope-rail__dump-picker',
              m(
                Select,
                {
                  className: 'pf-memscope-rail__select',
                  value: activeDump.ts.toString(),
                  title: 'Select Java heap dump snapshot',
                  onchange: (e: Event) => {
                    assertIsInstance(e.target, HTMLSelectElement);
                    const targetTs = BigInt(e.target.value);
                    const found = procDumps.find((d) => d.ts === targetTs);
                    if (found !== undefined) {
                      onSelectHeapDump(found);
                    }
                  },
                },
                procDumps.map((d, idx) => {
                  const offset = Time.diff(d.ts, trace.traceInfo.start);
                  return m(
                    'option',
                    {value: d.ts.toString()},
                    `Dump #${idx + 1} · ${formatDuration(trace, offset)}`,
                  );
                }),
              ),
            ),
          procDumps.length === 0
            ? m('.pf-memscope-rail__empty', 'No Java heap dumps captured')
            : HEAP_NAV_ENTRIES.map((entry) =>
                railItem({
                  icon: entry.icon,
                  label: entry.label,
                  active: inHeap && activeHeapView === entry.view,
                  onclick: () => onSelectHeapView(entry.view),
                }),
              ),
        ]),

        // Section 3: Open dynamic inspectors (Flamegraph drill-downs & object
        // instances) so multi-object leak tracing never overflows a tab bar.
        inspectorCount > 0 &&
          m('.pf-memscope-rail__section', [
            m('.pf-memscope-rail__section-header', [
              m('span.pf-memscope-rail__section-title', 'Inspectors'),
              inspectorCount > 1
                ? m(
                    'button.pf-memscope-rail__clear-btn',
                    {
                      type: 'button',
                      title: 'Close all open inspectors',
                      onclick: () => hdeSession.closeAllDynamicTabs(inHeap),
                    },
                    'Clear all',
                  )
                : m(
                    'span.pf-memscope-rail__section-meta',
                    String(inspectorCount),
                  ),
            ]),
            fgTabs.map((fg) => {
              const isActive =
                activeFg !== null &&
                activeFg.pathHashes === fg.pathHashes &&
                activeFg.isDominator === fg.isDominator;
              return railItem({
                icon: 'filter_alt',
                label: 'Flamegraph objects',
                badge:
                  fg.count !== null ? fg.count.toLocaleString() : undefined,
                active: isActive,
                onclick: () =>
                  hdeSession.navigate('flamegraph-objects', {
                    pathHashes: fg.pathHashes,
                    isDominator: fg.isDominator,
                  }),
                onClose: () =>
                  hdeSession.closeFlamegraph(
                    fg.pathHashes,
                    fg.isDominator,
                    inHeap,
                  ),
              });
            }),
            instTabs.map((obj) =>
              railItem({
                icon: 'data_object',
                label: obj.label,
                mono: true,
                title: obj.label,
                active: activeObjId === obj.objId,
                onclick: () => hdeSession.navigate('object', {id: obj.objId}),
                onClose: () => hdeSession.closeInstanceTab(obj.objId, inHeap),
              }),
            ),
          ]),
      ]),
    ]);
  }

  private renderCollapsedRail(
    trace: Trace,
    selectedProc: ProcMemStat | undefined,
    activeTab: MemoryTab,
    activeDump: HeapDump | null,
    hdeSession: HeapDumpExplorerSession,
    inspectorCount: number,
    onSelectProcessTab: (tab: 'summary' | 'smaps') => void,
    onSelectHeapView: (view: StaticHeapView) => void,
  ): m.Children {
    const smapsMissing =
      selectedProc !== undefined && selectedProc.smapsSnapshots === 0;
    const heapMissing =
      selectedProc !== undefined && selectedProc.heapDumps === 0;
    const inHeap = activeTab === 'heap';
    const activeHeapView = inHeap ? hdeSession.nav.view : undefined;
    const dumpSuffix =
      activeDump !== null
        ? ` (${formatDuration(trace, Time.diff(activeDump.ts, trace.traceInfo.start))})`
        : '';

    return m('aside.pf-memscope-rail.pf-memscope-rail--collapsed', [
      m(
        '.pf-memscope-rail__collapsed-top',
        m(Button, {
          icon: Icons.GoForward,
          compact: true,
          title: 'Expand memory navigation rail',
          onclick: () => {
            this.collapsed = false;
          },
        }),
      ),
      m('.pf-memscope-rail__collapsed-nav', [
        collapsedRailIcon({
          icon: 'dashboard',
          title: 'Process Memory: Overview',
          active: activeTab === 'summary',
          onclick: () => onSelectProcessTab('summary'),
        }),
        collapsedRailIcon({
          icon: 'table_rows',
          title: smapsMissing
            ? 'Smaps (no snapshots for this process)'
            : 'Process Memory: Smaps',
          active: activeTab === 'smaps',
          disabled: smapsMissing,
          onclick: () => onSelectProcessTab('smaps'),
        }),
        m('.pf-memscope-rail__collapsed-divider'),
        heapMissing
          ? collapsedRailIcon({
              icon: 'account_tree',
              title: 'Java Heap (no heap dumps for this process)',
              disabled: true,
            })
          : HEAP_NAV_ENTRIES.map((entry) =>
              collapsedRailIcon({
                icon: entry.icon,
                title: `Java Heap: ${entry.label}${dumpSuffix}`,
                active: inHeap && activeHeapView === entry.view,
                onclick: () => onSelectHeapView(entry.view),
              }),
            ),
        inspectorCount > 0 && [
          m('.pf-memscope-rail__collapsed-divider'),
          m(
            'button.pf-memscope-rail__collapsed-inspectors',
            {
              type: 'button',
              title: `${inspectorCount} open inspector${inspectorCount > 1 ? 's' : ''} (click to expand rail)`,
              className: classNames(
                inHeap &&
                  (activeHeapView === 'object' ||
                    activeHeapView === 'flamegraph-objects') &&
                  'pf-memscope-rail__item--active',
              ),
              onclick: () => {
                this.collapsed = false;
              },
            },
            String(inspectorCount),
          ),
        ],
      ]),
    ]);
  }
}

interface RailItemOpts {
  readonly icon: string;
  readonly label: string;
  readonly badge?: string;
  readonly active?: boolean;
  readonly disabled?: boolean;
  readonly mono?: boolean;
  readonly title?: string;
  readonly onclick?: () => void;
  readonly onClose?: () => void;
}

function railItem(opts: RailItemOpts): m.Child {
  return m(
    '.pf-memscope-rail__item',
    {
      'className': classNames(
        opts.active && 'pf-memscope-rail__item--active',
        opts.disabled && 'pf-memscope-rail__item--disabled',
      ),
      'role': 'button',
      'tabindex': opts.disabled ? -1 : 0,
      'aria-current': opts.active ? 'page' : undefined,
      'aria-disabled': opts.disabled ? 'true' : undefined,
      'title': opts.title,
      'onclick': opts.disabled ? undefined : opts.onclick,
      'onkeydown': opts.disabled
        ? undefined
        : (e: KeyboardEvent) => {
            if (e.key === 'Enter' || e.key === ' ') {
              e.preventDefault();
              opts.onclick?.();
            }
          },
      'onauxclick':
        !opts.disabled && opts.onClose
          ? (e: MouseEvent) => {
              if (e.button === 1) {
                e.preventDefault();
                opts.onClose?.();
              }
            }
          : undefined,
    },
    [
      m(Icon, {icon: opts.icon, className: 'pf-memscope-rail__item-icon'}),
      m(
        'span.pf-memscope-rail__item-label',
        {
          className: classNames(
            opts.mono && 'pf-memscope-rail__item-label--mono',
          ),
        },
        opts.label,
      ),
      opts.badge !== undefined &&
        m('span.pf-memscope-rail__item-badge', opts.badge),
      opts.onClose !== undefined &&
        m(Button, {
          className: 'pf-memscope-rail__item-close',
          icon: Icons.Close,
          compact: true,
          title: 'Close inspector',
          onclick: (e: Event) => {
            e.stopPropagation();
            opts.onClose?.();
          },
        }),
    ],
  );
}

function collapsedRailIcon(opts: {
  readonly icon: string;
  readonly title: string;
  readonly active?: boolean;
  readonly disabled?: boolean;
  readonly onclick?: () => void;
}): m.Child {
  return m(
    'button.pf-memscope-rail__collapsed-btn',
    {
      type: 'button',
      title: opts.title,
      disabled: opts.disabled,
      className: classNames(
        opts.active && 'pf-memscope-rail__item--active',
        opts.disabled && 'pf-memscope-rail__item--disabled',
      ),
      onclick: opts.disabled ? undefined : opts.onclick,
    },
    m(Icon, {icon: opts.icon}),
  );
}

function sourcePill(
  kind: 'smaps' | 'dump' | 'native',
  count: number,
  label: string,
): m.Child {
  return m(
    'span.pf-memscope-rail__source',
    {className: classNames(count === 0 && 'pf-memscope-rail__source--empty')},
    [
      m(`span.pf-memscope-capture__dot.pf-memscope-capture__dot--${kind}`),
      `${count} ${label}`,
    ],
  );
}

function procOptionLabel(p: ProcMemStat): string {
  return `${p.procName} (pid ${p.pid})`;
}
