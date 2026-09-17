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
import {AsyncMemo} from '../../../../base/async_memo';
import type {Setting} from '../../../../public/settings';
import type {Trace} from '../../../../public/trace';
import {Button, ButtonGroup, ButtonVariant} from '../../../../widgets/button';
import {Intent} from '../../../../widgets/common';
import {EmptyState} from '../../../../widgets/empty_state';
import {MenuItem, PopupMenu} from '../../../../widgets/menu';
import {PopupPosition} from '../../../../widgets/popup';
import {Select} from '../../../../widgets/select';
import {Callout} from '../../components/callout';
import {Page} from '../../components/page';
import {PreviewBanner} from '../../components/preview_banner';
import {MemoryOverviewTab, ProcessMemDetails} from './proc_mem_overview';
import {
  loadProcessMemoryStats,
  type ProcMemStat,
  type ProcWithMem,
} from './proc_mem_stats';
import './memory_overview_page.scss';

export interface MemoryOverviewPageAttrs {
  readonly trace: Trace;
  readonly upid?: number;
  readonly tab: MemoryOverviewTab;
  readonly autoNavigated: boolean;
  readonly hdeAvailable: boolean;
  readonly openByDefault: Setting<boolean>;
  readonly hideDefaultChangedHint: Setting<boolean>;
  readonly onUpidChange: (upid: number) => void;
  readonly onTabChange: (tab: MemoryOverviewTab) => void;
}

export class MemoryOverviewPage implements m.Component<MemoryOverviewPageAttrs> {
  private readonly slot = new AsyncMemo<ProcWithMem>();

  view({attrs}: m.Vnode<MemoryOverviewPageAttrs>) {
    const {
      trace,
      upid,
      tab,
      autoNavigated,
      hdeAvailable,
      openByDefault,
      hideDefaultChangedHint,
      onUpidChange,
      onTabChange,
    } = attrs;

    return m(
      Page,
      m(Page.Title, 'Memory Overview'),
      m(
        Page.Subtitle,
        'Memory triage: smaps owns the total, the native and Java ' +
          'profilers explain what is inside.',
      ),
      this.renderDefaultChangedHint(
        trace,
        autoNavigated,
        hdeAvailable,
        openByDefault,
        hideDefaultChangedHint,
      ),
      m(PreviewBanner, {app: trace}),
      this.renderPageContent(trace, upid, tab, onUpidChange, onTabChange),
    );
  }

  private renderDefaultChangedHint(
    trace: Trace,
    autoNavigated: boolean,
    hdeAvailable: boolean,
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
          'Memory Overview is now the default page when opening traces with ' +
            'smaps snapshots.',
        ),
        m(
          '.pf-memscope-default-page-callout__actions',
          m(
            ButtonGroup,
            {className: 'pf-memscope-default-page-callout__split-button'},
            m(Button, {
              label: 'Back to Heapdump Explorer',
              icon: 'arrow_back',
              variant: ButtonVariant.Filled,
              disabled: !hdeAvailable,
              title: hdeAvailable
                ? undefined
                : 'No Java heap dumps are available in this trace',
              onclick: () => trace.navigate('#!/heapdump'),
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

  private renderPageContent(
    trace: Trace,
    upid: number | undefined,
    tab: MemoryOverviewTab,
    onUpidChange: (upid: number) => void,
    onTabChange: (tab: MemoryOverviewTab) => void,
  ) {
    const procsWithMemResult = this.slot.use({
      key: '',
      compute: () => loadProcessMemoryStats(trace.engine),
    });

    const procs = procsWithMemResult.data;
    if (!procs) {
      return m(EmptyState, {icon: 'hourglass', title: 'Loading processes...'});
    }

    if (procs.length === 0) {
      return m(EmptyState, 'No processes with memory in this trace');
    }

    const selectedUpid = upid;

    return [
      m('.pf-memscope-process-select', [
        m('span.pf-memscope-process-select__label', 'Process'),
        m(
          Select,
          {
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
      selectedUpid === undefined || Number.isNaN(selectedUpid)
        ? m('', 'Unable to parse upid from url')
        : m(ProcessMemDetails, {trace, upid: selectedUpid, tab, onTabChange}),
    ];
  }
}

function procOptionLabel(p: ProcMemStat): string {
  const parts: string[] = [];
  if (p.heapDumps > 0) parts.push(`${p.heapDumps} java_hprof`);
  if (p.nativeDumps > 0) parts.push(`${p.nativeDumps} heapprofd`);
  if (p.smapsSnapshots > 0) parts.push(`${p.smapsSnapshots} smaps`);
  return parts.length > 0 ? `${p.procName} (${parts.join(', ')})` : p.procName;
}
