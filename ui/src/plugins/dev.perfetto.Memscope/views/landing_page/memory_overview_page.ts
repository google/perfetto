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
import {Gate} from '../../../../base/mithril_utils';
import {Time, type time} from '../../../../base/time';
import type {Setting} from '../../../../public/settings';
import type {Trace} from '../../../../public/trace';
import {Button, ButtonGroup, ButtonVariant} from '../../../../widgets/button';
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
import {EventTimeRibbon} from './event_ribbon';
import {
  type CaptureInfo,
  loadCaptureInfo,
  type MemoryWorkbenchSection,
  ProcessMemDetails,
} from './proc_mem_overview';
import {loadProcessMemoryStats, type ProcWithMem} from './proc_mem_stats';
import {HEAP_LENSES, ScopeRail} from './scope_rail';
import type {MemSelection} from './selection';
import {SmapsDetail} from './smaps_detail';
import {NativeSection} from './summary/native_section';
import './memory_overview_page.scss';

export interface MemoryOverviewPageAttrs {
  readonly trace: Trace;
  readonly upid?: number;
  readonly section: MemoryWorkbenchSection;
  readonly hdeSession?: HeapDumpExplorerSession;
  readonly hdeSubpage?: string;
  readonly autoNavigated: boolean;
  readonly openByDefault: Setting<boolean>;
  readonly hideDefaultChangedHint: Setting<boolean>;
  readonly onUpidChange: (upid: number) => void;
  readonly onSelectSection: (section: MemoryWorkbenchSection) => void;
  readonly onSelectHeapView: (view: NavView, preferredTs?: time) => void;
  readonly onSelectDump: (dump: HeapDump, jumpToHeap: boolean) => void;
  readonly onOpenJavaHeap: (
    ts: time,
    view: NavView,
    params?: Record<string, unknown>,
  ) => void;
}

function formatOffsetSec(trace: Trace, ts: time): string {
  return `${(Number(Time.diff(ts, trace.traceInfo.start)) / 1e9).toFixed(1)}s`;
}

function heapViewDisplayTitle(session: HeapDumpExplorerSession): string {
  const nav = session.nav;
  if (nav.view === 'object') {
    const tab = session.instanceTabs.find((t) => t.objId === nav.params.id);
    return tab !== undefined
      ? tab.label
      : `Object 0x${nav.params.id.toString(16)}`;
  }
  if (nav.view === 'flamegraph-objects') {
    const active = session.activeFlamegraph;
    const tab =
      active !== null
        ? session.flamegraphTabs.find(
            (t) =>
              t.pathHashes === active.pathHashes &&
              t.isDominator === active.isDominator,
          )
        : undefined;
    return tab !== undefined && tab.count !== null
      ? `Flamegraph objects (${tab.count.toLocaleString()})`
      : 'Flamegraph objects';
  }
  return HEAP_LENSES.find((l) => l.view === nav.view)?.label ?? 'Heap Summary';
}

export class MemoryOverviewPage implements m.ClassComponent<MemoryOverviewPageAttrs> {
  private readonly procsSlot = new AsyncMemo<ProcWithMem>();
  private readonly captureSlot = new AsyncMemo<CaptureInfo>();
  private readonly selectionByUpid = new Map<number, MemSelection>();
  private railCollapsed = false;

  onremove() {
    this.procsSlot.dispose();
    this.captureSlot.dispose();
  }

  view({attrs}: m.Vnode<MemoryOverviewPageAttrs>) {
    const {
      trace,
      upid,
      section,
      hdeSession,
      hdeSubpage,
      autoNavigated,
      openByDefault,
      hideDefaultChangedHint,
      onUpidChange,
      onSelectSection,
      onSelectHeapView,
      onSelectDump,
      onOpenJavaHeap,
    } = attrs;

    const procs = this.procsSlot.use({
      key: trace.traceInfo.uuid,
      compute: () => loadProcessMemoryStats(trace.engine),
    }).data;

    if (procs === undefined) {
      return m(
        '.pf-memscope-page.pf-memwb',
        m(
          '.pf-memwb__empty',
          m(EmptyState, {icon: 'hourglass', title: 'Loading processes...'}),
        ),
      );
    }

    if (procs.length === 0) {
      return m(
        '.pf-memscope-page.pf-memwb',
        m(
          '.pf-memwb__empty',
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
    const smapsCount = capture?.smaps.samples ?? selectedProc?.smapsSnapshots;
    const nativeCount = capture?.native.samples ?? selectedProc?.nativeDumps;
    const dumpsCount =
      procDumps.length > 0
        ? procDumps.length
        : (capture?.dumps ?? selectedProc?.heapDumps);

    const smapsAvailable = smapsCount === undefined || smapsCount > 0;
    const nativeAvailable = nativeCount === undefined || nativeCount > 0;
    const heapAvailable = dumpsCount === undefined || dumpsCount > 0;

    let effectiveSection: MemoryWorkbenchSection = section;
    if (effectiveSection === 'smaps' && !smapsAvailable) {
      effectiveSection = 'overview';
    }
    if (effectiveSection === 'native' && !nativeAvailable) {
      effectiveSection = 'overview';
    }

    const currentSelection =
      validUpid !== undefined ? this.selectionByUpid.get(validUpid) : undefined;

    return m('.pf-memscope-page.pf-memwb', [
      !this.railCollapsed &&
        m(ScopeRail, {
          trace,
          procs,
          selectedProc,
          validUpid,
          capture,
          section: effectiveSection,
          procDumps,
          hdeSession,
          onUpidChange,
          onSelectSection,
          onSelectHeapView: (view) =>
            onSelectHeapView(view, currentSelection?.sel),
          onSelectDump: (dump) => onSelectDump(dump, true),
        }),

      m('.pf-memwb__main', [
        this.renderTopBar({
          trace,
          validUpid,
          section: effectiveSection,
          procDumps,
          hdeSession,
          selection: currentSelection,
          onSelectSection,
          onSelectHeapView: (view) =>
            onSelectHeapView(view, currentSelection?.sel),
          onSelectSmapsTs: (ts) => {
            if (validUpid === undefined) return;
            this.selectionByUpid.set(validUpid, {sel: ts});
            if (effectiveSection === 'heap') {
              onSelectSection('overview');
            }
          },
          onSelectHeapDump: (dump) => {
            if (validUpid !== undefined) {
              this.selectionByUpid.set(validUpid, {sel: dump.ts});
            }
            onSelectDump(dump, true);
          },
          onClearSelection:
            validUpid !== undefined
              ? () => this.selectionByUpid.delete(validUpid)
              : undefined,
        }),

        m('.pf-memwb__canvas', [
          captureError !== undefined &&
            m(
              '.pf-memwb__scroll',
              m(
                '.pf-memscope-page__content',
                m('p.pf-error', `Error: ${captureError}`),
              ),
            ),
          validUpid === undefined &&
            m(
              '.pf-memwb__empty',
              m(EmptyState, {
                icon: 'error_outline',
                title: 'Unable to resolve process from URL',
              }),
            ),
          validUpid !== undefined &&
            captureError === undefined && [
              m(
                Gate,
                {open: effectiveSection === 'overview'},
                m(
                  '.pf-memwb__scroll',
                  m('.pf-memscope-page__content', [
                    this.renderDefaultChangedHint(
                      trace,
                      autoNavigated,
                      openByDefault,
                      hideDefaultChangedHint,
                    ),
                    m(ProcessMemDetails, {
                      trace,
                      upid: validUpid,
                      capture,
                      selection: currentSelection,
                      onSelect: (sel: MemSelection) => {
                        this.selectionByUpid.set(validUpid, sel);
                      },
                      onInspectSmaps: smapsAvailable
                        ? () => onSelectSection('smaps')
                        : undefined,
                      onOpenJavaHeap: heapAvailable
                        ? onOpenJavaHeap
                        : undefined,
                    }),
                  ]),
                ),
              ),
              m(
                Gate,
                {open: effectiveSection === 'smaps'},
                m(
                  '.pf-memwb__scroll',
                  m(
                    '.pf-memscope-page__content',
                    m(SmapsDetail, {trace, upid: validUpid}),
                  ),
                ),
              ),
              m(
                Gate,
                {open: effectiveSection === 'native'},
                m(
                  '.pf-memwb__scroll',
                  m(
                    '.pf-memscope-page__content',
                    m(NativeSection, {
                      trace,
                      upid: validUpid,
                      selTs: currentSelection?.sel,
                      baseTs: currentSelection?.base,
                    }),
                  ),
                ),
              ),
              m(
                Gate,
                {open: effectiveSection === 'heap'},
                this.renderHeapCanvas({
                  procs,
                  procDumps,
                  hdeSession,
                  hdeSubpage,
                  active: effectiveSection === 'heap',
                  onUpidChange,
                  onSelectSection,
                }),
              ),
            ],
        ]),
      ]),
    ]);
  }

  private renderTopBar(opts: {
    readonly trace: Trace;
    readonly validUpid?: number;
    readonly section: MemoryWorkbenchSection;
    readonly procDumps: ReadonlyArray<HeapDump>;
    readonly hdeSession?: HeapDumpExplorerSession;
    readonly selection?: MemSelection;
    readonly onSelectSection: (section: MemoryWorkbenchSection) => void;
    readonly onSelectHeapView: (view: NavView) => void;
    readonly onSelectSmapsTs: (ts: time) => void;
    readonly onSelectHeapDump: (dump: HeapDump) => void;
    readonly onClearSelection?: () => void;
  }): m.Children {
    const {
      trace,
      validUpid,
      section,
      procDumps,
      hdeSession,
      selection,
      onSelectSection,
      onSelectHeapView,
      onSelectSmapsTs,
      onSelectHeapDump,
      onClearSelection,
    } = opts;

    const crumbs: m.Children[] = [];
    const sep = () =>
      m(Icon, {icon: 'chevron_right', className: 'pf-memwb-crumbs__sep'});

    if (section === 'heap' && hdeSession !== undefined) {
      crumbs.push(
        m(
          'button.pf-memwb-crumbs__link',
          {type: 'button', onclick: () => onSelectSection('overview')},
          'Process',
        ),
        sep(),
        m(
          'button.pf-memwb-crumbs__link',
          {type: 'button', onclick: () => onSelectHeapView('overview')},
          'Java Heap',
        ),
      );
      const activeDump = hdeSession.activeDump;
      const dumpIdx = activeDump !== null ? procDumps.indexOf(activeDump) : -1;
      if (activeDump !== null && dumpIdx >= 0) {
        crumbs.push(
          sep(),
          m(
            'span.pf-memwb-crumbs__tag',
            `Dump #${dumpIdx + 1} · ${formatOffsetSec(trace, activeDump.ts)}`,
          ),
        );
      }
      crumbs.push(
        sep(),
        m('span.pf-memwb-crumbs__current', heapViewDisplayTitle(hdeSession)),
      );
    } else {
      const sectionLabel =
        section === 'smaps'
          ? 'Memory Map (Smaps)'
          : section === 'native'
            ? 'Native Allocations'
            : 'Overview';
      crumbs.push(
        m(
          'button.pf-memwb-crumbs__link',
          {type: 'button', onclick: () => onSelectSection('overview')},
          'Process',
        ),
        sep(),
        m('span.pf-memwb-crumbs__current', sectionLabel),
      );
    }

    return m('header.pf-memwb-topbar', [
      m('.pf-memwb-topbar__left', [
        m(Button, {
          icon: this.railCollapsed ? 'menu' : 'menu_open',
          variant: ButtonVariant.Minimal,
          compact: true,
          title: this.railCollapsed
            ? 'Show scope navigator'
            : 'Collapse scope navigator',
          onclick: () => {
            this.railCollapsed = !this.railCollapsed;
          },
        }),
        m('.pf-memwb-crumbs', crumbs),
      ]),
      m('.pf-memwb-topbar__right', [
        validUpid !== undefined &&
          m(EventTimeRibbon, {
            trace,
            upid: validUpid,
            procDumps,
            activeDump: hdeSession?.activeDump ?? null,
            selection,
            onSelectSmapsTs,
            onSelectHeapDump,
          }),
        selection !== undefined &&
          section !== 'heap' &&
          m('.pf-memwb-pin-pill', [
            m(
              'span',
              selection.base !== undefined
                ? `${formatOffsetSec(trace, selection.base)} → ${formatOffsetSec(trace, selection.sel)}`
                : `@ ${formatOffsetSec(trace, selection.sel)}`,
            ),
            onClearSelection !== undefined &&
              m(Button, {
                icon: 'close',
                compact: true,
                title: 'Reset snapshot filter',
                onclick: onClearSelection,
              }),
          ]),
      ]),
    ]);
  }

  private renderHeapCanvas(opts: {
    readonly procs: ProcWithMem;
    readonly procDumps: ReadonlyArray<HeapDump>;
    readonly hdeSession?: HeapDumpExplorerSession;
    readonly hdeSubpage?: string;
    readonly active: boolean;
    readonly onUpidChange: (upid: number) => void;
    readonly onSelectSection: (section: MemoryWorkbenchSection) => void;
  }): m.Children {
    const {
      procs,
      procDumps,
      hdeSession,
      hdeSubpage,
      active,
      onUpidChange,
      onSelectSection,
    } = opts;

    if (hdeSession === undefined || procDumps.length === 0) {
      const otherWithHeap = procs.find((p) => p.heapDumps > 0);
      return m(
        '.pf-memwb__empty',
        m(
          EmptyState,
          {
            icon: 'account_tree',
            title: 'No Java heap dumps for this process',
          },
          [
            m(
              'p',
              'Capture a java_hprof dump to inspect retained objects, dominators, and bitmaps.',
            ),
            m('.pf-memwb-empty-actions', [
              m(Button, {
                label: 'Back to Process Overview',
                icon: 'dashboard',
                variant: ButtonVariant.Filled,
                onclick: () => onSelectSection('overview'),
              }),
              otherWithHeap !== undefined &&
                m(Button, {
                  label: `Inspect ${otherWithHeap.procName}`,
                  icon: 'account_tree',
                  variant: ButtonVariant.Outlined,
                  onclick: () => onUpidChange(otherWithHeap.upid),
                }),
            ]),
          ],
        ),
      );
    }

    return m(
      '.pf-memwb__heap',
      m(HeapDumpPage, {
        session: hdeSession,
        subpage: hdeSubpage,
        embedded: true,
        hideTabBar: true,
        active,
      }),
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
          'Memory Workbench is now the default page when opening traces with ' +
            'memory snapshots or heap dumps. Use the left scope rail or the ' +
            'event time ribbon above to move between Process, Smaps, Native, ' +
            'and Java Heap views.',
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
