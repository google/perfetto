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
import {assertIsInstance} from '../../../base/assert';
import {AsyncMemo} from '../../../base/async_memo';
import type {Setting} from '../../../public/settings';
import type {Trace} from '../../../public/trace';
import {Button, ButtonGroup, ButtonVariant} from '../../../widgets/button';
import {Intent} from '../../../widgets/common';
import {EmptyState} from '../../../widgets/empty_state';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {PopupPosition} from '../../../widgets/popup';
import {Select} from '../../../widgets/select';
import {Spinner} from '../../../widgets/spinner';
import {Tabs, type TabsTab} from '../../../widgets/tabs';
import {
  buildHeapDumpTabs,
  heapDumpActiveTabKey,
  renderDumpSelector,
  type TabActions,
} from '../../com.android.HeapDumpExplorer/heap_dump_views';
import {
  isProcessView,
  subpageToMemoryLocation,
} from '../../com.android.HeapDumpExplorer/nav_state';
import * as hdeQueries from '../../com.android.HeapDumpExplorer/queries';
import type {HeapDumpExplorerSession} from '../../com.android.HeapDumpExplorer/session';
import {Callout} from '../components/callout';
import {Page, SubPage} from '../components/page';
import {ProcessMemDetails} from '../views/landing_page/proc_mem_overview';
import {
  type ProcMemStat,
  type ProcWithMem,
  loadProcessMemoryStats,
  pickBestProc,
} from '../views/landing_page/proc_mem_stats';
import type {HeapDumpLink} from '../views/landing_page/section_widgets';
import {SmapsDetail} from '../views/landing_page/smaps_detail';
import {memoryExplorerHref} from './route';
import '../views/landing_page/memory_overview_page.scss';
import './styles.scss';

export interface MemoryExplorerPageAttrs {
  readonly trace: Trace;
  readonly session: HeapDumpExplorerSession;
  readonly subpage: string | undefined;
  readonly autoNavigated: boolean;
  readonly openByDefault: Setting<boolean>;
  readonly hideDefaultChangedHint: Setting<boolean>;
}

const NO_CLASSES: ReadonlySet<string> = new Set();

// One process-scoped page combining the smaps/native/Java summary with the
// heap dump views. Route: /memory/<upid>/<nav subpage>.
export class MemoryExplorerPage implements m.ClassComponent<MemoryExplorerPageAttrs> {
  private readonly procsMemo = new AsyncMemo<ProcWithMem>();
  private readonly mergedClassesMemo = new AsyncMemo<ReadonlySet<string>>();

  onremove() {
    this.procsMemo.dispose();
    this.mergedClassesMemo.dispose();
  }

  view({attrs}: m.Vnode<MemoryExplorerPageAttrs>): m.Children {
    const {trace, session, subpage} = attrs;
    const procs = this.procsMemo.use({
      key: trace.traceInfo.uuid,
      compute: () => loadProcessMemoryStats(trace.engine),
    }).data;
    if (procs === undefined) {
      return m(EmptyState, {icon: 'hourglass', title: 'Loading processes...'});
    }

    const loc = subpageToMemoryLocation(subpage);
    const proc = procs.find((p) => p.upid === loc.upid);
    if (proc === undefined) {
      const best = pickBestProc(procs);
      if (best === undefined) {
        return m(EmptyState, {title: 'No processes with memory in this trace'});
      }
      location.replace(memoryExplorerHref(best.upid));
      return m(EmptyState, {icon: 'hourglass', title: 'Loading process...'});
    }

    const upid = proc.upid;
    session.selectProcess(upid);
    session.syncFromSubpage(subpage?.replace(/^\/?\d+\/?/, '') || 'summary');
    const dump = session.activeDump;
    const hasJava = dump !== null;
    const hasSmaps = proc.smapsSnapshots > 0;
    let nav = session.nav;
    if (
      (!hasJava && !isProcessView(nav.view)) ||
      (nav.view === 'smaps' && !hasSmaps)
    ) {
      nav = {view: 'summary', params: {}};
    }
    if (hasJava) {
      session.syncInstanceTabFromNav();
      session.syncFlamegraphTabFromNav();
    }

    const heapDumpLink: HeapDumpLink = (ts, target) => ({
      href: memoryExplorerHref(upid, target),
      onclick: () => {
        const d = session.dumpsForUpid(upid).find((x) => x.ts === ts);
        if (d !== undefined) session.selectDump(d);
      },
    });

    const tabs: TabsTab[] = [
      {
        key: 'summary',
        title: 'Summary',
        content: m(
          Page,
          m(ProcessMemDetails, {trace, upid, tab: 'summary', heapDumpLink}),
        ),
      },
    ];
    if (hasSmaps) {
      tabs.push({
        key: 'smaps',
        title: 'Smaps',
        content: m(Page, m(SubPage, m(SmapsDetail, {trace, upid}))),
      });
    }
    const actions = new Map<string, TabActions>();
    for (const t of tabs) {
      actions.set(t.key, {select: () => session.navigate(t.key as 'summary')});
    }

    let activeKey: string = nav.view;
    if (dump !== null) {
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
        'Java heap',
      );
      tabs.push(...hde.tabs);
      hde.actions.forEach((a, k) => actions.set(k, a));
      if (!isProcessView(nav.view)) activeKey = heapDumpActiveTabKey(session);
    } else if (proc.heapDumps > 0) {
      // Dumps exist but the session hasn't loaded them yet.
      tabs.push({
        key: 'loading',
        title: 'Java heap',
        content: m('.pf-hde-loading', m(Spinner, {easing: true})),
      });
    }

    return m(
      '.pf-hde-page',
      this.renderDefaultChangedHint(attrs),
      m('.pf-memory-explorer__header', [
        m('span.pf-memory-explorer__title', 'Memory Explorer'),
        this.renderProcessSelect(procs, upid),
        renderDumpSelector(trace, session.dumpsForUpid(upid), dump, (d) =>
          session.selectDump(d),
        ),
      ]),
      m(
        'main',
        {class: 'pf-hde-page__tabs'},
        m(Tabs, {
          key: `${upid}:${dump?.ts ?? ''}`,
          tabs: tabs.map((t) => ({...t, lazy: true})),
          activeTabKey: activeKey,
          onTabChange: (key: string) => actions.get(key)?.select(),
          onTabClose: (key: string) => actions.get(key)?.close?.(),
        }),
      ),
    );
  }

  private renderProcessSelect(procs: ProcWithMem, upid: number): m.Children {
    return m('.pf-memscope-process-select', [
      m('span.pf-memscope-process-select__label', 'Process'),
      m(
        Select,
        {
          value: upid.toString(),
          onchange: (e: Event) => {
            assertIsInstance(e.target, HTMLSelectElement);
            location.href = memoryExplorerHref(Number(e.target.value));
          },
        },
        procs.map((p) =>
          m('option', {value: p.upid.toString()}, procOptionLabel(p)),
        ),
      ),
    ]);
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

function procOptionLabel(p: ProcMemStat): string {
  const parts: string[] = [];
  if (p.heapDumps > 0) parts.push(`${p.heapDumps} java_hprof`);
  if (p.nativeDumps > 0) parts.push(`${p.nativeDumps} heapprofd`);
  if (p.smapsSnapshots > 0) parts.push(`${p.smapsSnapshots} smaps`);
  return parts.length > 0 ? `${p.procName} (${parts.join(', ')})` : p.procName;
}
