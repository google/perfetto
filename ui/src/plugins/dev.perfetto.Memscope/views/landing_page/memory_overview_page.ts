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
import {Gate} from '../../../../base/mithril_utils';
import {Time} from '../../../../base/time';
import {formatDuration} from '../../../../components/time_utils';
import type {Setting} from '../../../../public/settings';
import type {Trace} from '../../../../public/trace';
import {Button, ButtonGroup, ButtonVariant} from '../../../../widgets/button';
import {Intent} from '../../../../widgets/common';
import {EmptyState} from '../../../../widgets/empty_state';
import {MenuItem, PopupMenu} from '../../../../widgets/menu';
import {PopupPosition} from '../../../../widgets/popup';
import {Select} from '../../../../widgets/select';
import {TabStrip} from '../../../../widgets/tab_strip';
import {HeapDumpPage} from '../../../com.android.HeapDumpExplorer/heap_dump_page';
import type {HeapDumpExplorerSession} from '../../../com.android.HeapDumpExplorer/session';
import {Callout} from '../../components/callout';
import {Page} from '../../components/page';
import {PreviewBanner} from '../../components/preview_banner';
import {
  formatMemorySubpage,
  MEMORY_PAGE_ROOT,
  type MemoryTab,
  resolveTabForProc,
} from './memory_route';
import {ProcessMemDetails} from './proc_mem_overview';
import type {ProcMemStat, ProcWithMem} from './proc_mem_stats';
import './memory_overview_page.scss';

export interface MemoryOverviewPageAttrs {
  readonly trace: Trace;
  readonly procs: ProcWithMem;
  readonly upid?: number;
  readonly tab: MemoryTab;
  readonly hdeSubpage?: string;
  readonly hdeSession: HeapDumpExplorerSession;
  readonly autoNavigated: boolean;
  readonly hdeAvailable: boolean;
  readonly openByDefault: Setting<boolean>;
  readonly hideDefaultChangedHint: Setting<boolean>;
  readonly onUpidChange: (upid: number) => void;
  readonly onTabChange: (tab: MemoryTab) => void;
}

export class MemoryOverviewPage implements m.ClassComponent<MemoryOverviewPageAttrs> {
  // Lazily mount each scope body on first visit so entering directly into Java
  // Heap doesn't run Memscope SQL queries, and vice versa. Once mounted, Gate
  // keeps each scope's state alive across tab switches.
  private activatedOverview = false;
  private activatedHeap = false;

  view({attrs}: m.Vnode<MemoryOverviewPageAttrs>) {
    const {
      trace,
      procs,
      upid,
      tab,
      hdeSubpage,
      hdeSession,
      autoNavigated,
      hdeAvailable,
      openByDefault,
      hideDefaultChangedHint,
      onUpidChange,
      onTabChange,
    } = attrs;

    if (procs.length === 0) {
      return m(EmptyState, {
        title: 'No processes with memory in this trace',
        fillHeight: true,
      });
    }

    const validUpid =
      upid !== undefined && !Number.isNaN(upid) ? upid : undefined;
    const selectedProc =
      validUpid !== undefined
        ? procs.find((p) => p.upid === validUpid)
        : undefined;
    const activeTab = resolveTabForProc(tab, selectedProc);

    if (activeTab === 'heap' && validUpid !== undefined) {
      hdeSession.syncUpid(validUpid);
      this.activatedHeap = true;
    } else if (validUpid !== undefined) {
      this.activatedOverview = true;
    }

    return m('.pf-memscope-unified', [
      this.renderHeader(
        trace,
        procs,
        validUpid,
        selectedProc,
        activeTab,
        hdeSession,
        onUpidChange,
        onTabChange,
      ),
      m('.pf-memscope-unified__body', [
        validUpid === undefined
          ? m(EmptyState, {
              title: 'Unable to parse process from URL',
              fillHeight: true,
            })
          : [
              m(
                Gate,
                {open: activeTab === 'summary' || activeTab === 'smaps'},
                this.activatedOverview &&
                  m(
                    Page,
                    m(Page.Title, 'Memory Overview'),
                    m(
                      Page.Subtitle,
                      'Memory triage: smaps owns the total, the native and ' +
                        'Java profilers explain what is inside.',
                    ),
                    this.renderDefaultChangedHint(
                      trace,
                      procs,
                      selectedProc,
                      autoNavigated,
                      hdeAvailable,
                      openByDefault,
                      hideDefaultChangedHint,
                      onTabChange,
                    ),
                    m(PreviewBanner, {app: trace}),
                    m(ProcessMemDetails, {
                      trace,
                      upid: validUpid,
                      tab: activeTab === 'smaps' ? 'smaps' : 'summary',
                    }),
                  ),
              ),
              m(
                Gate,
                {open: activeTab === 'heap'},
                this.activatedHeap &&
                  m(HeapDumpPage, {
                    session: hdeSession,
                    subpage: hdeSubpage,
                    hideDumpSelector: true,
                  }),
              ),
            ],
      ]),
    ]);
  }

  private renderHeader(
    trace: Trace,
    procs: ProcWithMem,
    selectedUpid: number | undefined,
    selectedProc: ProcMemStat | undefined,
    activeTab: MemoryTab,
    hdeSession: HeapDumpExplorerSession,
    onUpidChange: (upid: number) => void,
    onTabChange: (tab: MemoryTab) => void,
  ): m.Children {
    const smapsMissing =
      selectedProc !== undefined && selectedProc.smapsSnapshots === 0;
    const heapMissing =
      selectedProc !== undefined && selectedProc.heapDumps === 0;

    const tabs: ReadonlyArray<{
      readonly key: MemoryTab;
      readonly label: string;
      readonly icon: string;
      readonly disabled?: boolean;
      readonly tooltip?: string;
    }> = [
      {key: 'summary', label: 'Summary', icon: 'description'},
      {
        key: 'smaps',
        label: 'Smaps',
        icon: 'table_rows',
        disabled: smapsMissing,
        tooltip: smapsMissing
          ? 'No smaps data available for this process'
          : undefined,
      },
      {
        key: 'heap',
        label: 'Java Heap',
        icon: 'account_tree',
        disabled: heapMissing,
        tooltip: heapMissing
          ? 'No Java heap dumps available for this process'
          : undefined,
      },
    ];

    return m('.pf-memscope-unified__header', [
      m('.pf-memscope-unified__process', [
        m('span.pf-memscope-unified__label', 'Process'),
        m(
          Select,
          {
            className: 'pf-memscope-unified__select',
            value: selectedUpid?.toString(),
            onchange: (e: Event) => {
              assertIsInstance(e.target, HTMLSelectElement);
              onUpidChange(Number(e.target.value));
            },
          },
          procs.map((p) =>
            m('option', {value: p.upid.toString()}, procOptionLabel(p)),
          ),
        ),
      ]),
      m('.pf-memscope-unified__divider'),
      m(
        TabStrip,
        {
          variant: 'underline',
          className: 'pf-memscope-unified__tabs',
        },
        tabs.map((t) =>
          m(
            TabStrip.Tab,
            {
              key: t.key,
              active: t.key === activeTab,
              disabled: t.disabled,
              icon: t.icon,
              title: t.tooltip,
              onclick: () => onTabChange(t.key),
            },
            t.label,
          ),
        ),
      ),
      activeTab === 'heap' &&
        selectedUpid !== undefined &&
        this.renderDumpControl(trace, selectedUpid, hdeSession),
    ]);
  }

  private renderDumpControl(
    trace: Trace,
    upid: number,
    hdeSession: HeapDumpExplorerSession,
  ): m.Children {
    const procDumps = hdeSession.dumpsForUpid(upid);
    const active = hdeSession.activeDump;
    if (procDumps.length === 0 || active === null || active.upid !== upid) {
      return undefined;
    }

    const activeOffset = Time.diff(active.ts, trace.traceInfo.start);
    if (procDumps.length === 1) {
      return m(
        '.pf-memscope-unified__right',
        m(
          'span.pf-memscope-unified__dump-meta',
          `Dump @ ${formatDuration(trace, activeOffset)}`,
        ),
      );
    }

    const activeIdx = Math.max(0, procDumps.indexOf(active));
    return m('.pf-memscope-unified__right', [
      m('span.pf-memscope-unified__label', 'Dump'),
      m(
        PopupMenu,
        {
          trigger: m(Button, {
            label: `#${activeIdx + 1} · ${formatDuration(trace, activeOffset)}`,
            icon: 'schedule',
            rightIcon: 'arrow_drop_down',
            variant: ButtonVariant.Outlined,
            compact: true,
          }),
          position: PopupPosition.BottomEnd,
        },
        procDumps.map((d, idx) => {
          const offset = Time.diff(d.ts, trace.traceInfo.start);
          return m(MenuItem, {
            label: `#${idx + 1} — ${formatDuration(trace, offset)}`,
            active: d === active,
            onclick: () => hdeSession.selectDump(d),
          });
        }),
      ),
    ]);
  }

  private renderDefaultChangedHint(
    trace: Trace,
    procs: ProcWithMem,
    selectedProc: ProcMemStat | undefined,
    autoNavigated: boolean,
    hdeAvailable: boolean,
    openByDefault: Setting<boolean>,
    hideDefaultChangedHint: Setting<boolean>,
    onTabChange: (tab: MemoryTab) => void,
  ): m.Children {
    if (
      !autoNavigated ||
      !openByDefault.get() ||
      hideDefaultChangedHint.get()
    ) {
      return undefined;
    }

    const firstHeapProc = procs.find((p) => p.heapDumps > 0);
    const canOpenHeap = hdeAvailable && firstHeapProc !== undefined;

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
          'Memory Overview is now the default view when opening traces with ' +
            'smaps snapshots.',
        ),
        m(
          '.pf-memscope-default-page-callout__actions',
          m(
            ButtonGroup,
            {className: 'pf-memscope-default-page-callout__split-button'},
            canOpenHeap
              ? m(Button, {
                  label: 'Switch to Java Heap',
                  icon: 'account_tree',
                  variant: ButtonVariant.Filled,
                  onclick: () => {
                    if (
                      selectedProc !== undefined &&
                      selectedProc.heapDumps > 0
                    ) {
                      onTabChange('heap');
                    } else {
                      trace.navigate(
                        `#!${MEMORY_PAGE_ROOT}/${formatMemorySubpage(firstHeapProc.upid, 'heap')}`,
                      );
                    }
                  },
                })
              : m(Button, {
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
              canOpenHeap &&
                m(MenuItem, {
                  label: 'Back to Timeline',
                  icon: 'arrow_back',
                  onclick: () => trace.navigate('#!/viewer'),
                }),
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

function procOptionLabel(p: ProcMemStat): string {
  const parts: string[] = [];
  if (p.heapDumps > 0) parts.push(`${p.heapDumps} java_hprof`);
  if (p.nativeDumps > 0) parts.push(`${p.nativeDumps} heapprofd`);
  if (p.smapsSnapshots > 0) parts.push(`${p.smapsSnapshots} smaps`);
  return parts.length > 0 ? `${p.procName} (${parts.join(', ')})` : p.procName;
}
