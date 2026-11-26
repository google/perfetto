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
import {Gate} from '../../../../base/mithril_utils';
import type {Setting} from '../../../../public/settings';
import type {Trace} from '../../../../public/trace';
import {Button, ButtonGroup, ButtonVariant} from '../../../../widgets/button';
import {Intent} from '../../../../widgets/common';
import {EmptyState} from '../../../../widgets/empty_state';
import {MenuItem, PopupMenu} from '../../../../widgets/menu';
import {PopupPosition} from '../../../../widgets/popup';
import {HeapDumpPage} from '../../../com.android.HeapDumpExplorer/heap_dump_page';
import type {NavView} from '../../../com.android.HeapDumpExplorer/nav_state';
import type {HeapDumpExplorerSession} from '../../../com.android.HeapDumpExplorer/session';
import type {HeapDump} from '../../../com.android.HeapDumpExplorer/types';
import {Callout} from '../../components/callout';
import {Page} from '../../components/page';
import {PreviewBanner} from '../../components/preview_banner';
import {MemoryRail} from './memory_rail';
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
  readonly onTabChange: (tab: MemoryTab, hdeSubpage?: string) => void;
}

export class MemoryOverviewPage implements m.ClassComponent<MemoryOverviewPageAttrs> {
  // Lazily mount each scope body on first visit so entering directly into Java
  // Heap doesn't run Memscope SQL queries, and vice versa. Once mounted, Gate
  // keeps each scope's state alive across view switches.
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

    if (validUpid !== undefined) {
      hdeSession.syncUpid(validUpid);
      if (activeTab === 'heap') {
        // Sync URL-driven HDE state before rendering MemoryRail so the rail
        // highlights the exact active view / new inspector on the same frame.
        hdeSession.syncFromSubpage(hdeSubpage);
        hdeSession.syncInstanceTabFromNav();
        hdeSession.syncFlamegraphTabFromNav();
        this.activatedHeap = true;
      } else {
        this.activatedOverview = true;
      }
    }

    return m('.pf-memscope-workbench', [
      m(MemoryRail, {
        trace,
        procs,
        selectedUpid: validUpid,
        selectedProc,
        activeTab,
        hdeSession,
        onUpidChange,
        onSelectProcessTab: (processTab) => onTabChange(processTab),
        onSelectHeapView: (
          view: Exclude<NavView, 'object' | 'flamegraph-objects'>,
        ) => onTabChange('heap', view),
        onSelectHeapDump: (dump: HeapDump) => {
          if (activeTab === 'heap') {
            hdeSession.selectDump(dump);
          } else {
            hdeSession.setActiveDump(dump);
            onTabChange('heap');
          }
        },
      }),
      m('.pf-memscope-workbench__main', [
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
                    hideTabBar: true,
                  }),
              ),
            ],
      ]),
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
    onTabChange: (tab: MemoryTab, hdeSubpage?: string) => void,
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
