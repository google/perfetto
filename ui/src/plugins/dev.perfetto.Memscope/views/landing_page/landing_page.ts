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
import type {Engine} from '../../../../trace_processor/engine';
import {
  materializeRows,
  NUM,
  STR,
} from '../../../../trace_processor/query_result';
import {Button, ButtonGroup, ButtonVariant} from '../../../../widgets/button';
import {Intent} from '../../../../widgets/common';
import {EmptyState} from '../../../../widgets/empty_state';
import {MenuItem, PopupMenu} from '../../../../widgets/menu';
import {PopupPosition} from '../../../../widgets/popup';
import {Select} from '../../../../widgets/select';
import {Callout} from '../../components/callout';
import {Page} from '../../components/page';
import {PreviewBanner} from '../../components/preview_banner';
import './landing_page.scss';
import {ProcessMemDetails} from './proc_mem_overview';

// Per-process memory-capture counts, used to populate and score the process
// picker on the overview page.
interface ProcMemStat {
  readonly upid: number;
  readonly pid: number;
  readonly procName: string;
  readonly heapDumps: number;
  readonly smapsSnapshots: number;
  readonly nativeDumps: number;
}

export interface MemoryOverviewPageAttrs {
  readonly trace: Trace;
  readonly subpage: string | undefined;
  readonly autoNavigated: boolean;
  readonly hdeAvailable: boolean;
  readonly openByDefault: Setting<boolean>;
  readonly hideDefaultChangedHint: Setting<boolean>;
  readonly onSubpageChange: (subpage: string) => void;
}

type ProcWithMem = readonly ProcMemStat[];

export class MemoryOverviewPage implements m.Component<MemoryOverviewPageAttrs> {
  private readonly slot = new AsyncMemo<ProcWithMem>();

  view({attrs}: m.Vnode<MemoryOverviewPageAttrs>) {
    const {
      trace,
      subpage,
      autoNavigated,
      hdeAvailable,
      openByDefault,
      hideDefaultChangedHint,
      onSubpageChange,
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
      this.renderPageContent(trace, subpage, onSubpageChange),
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
    subpage: string | undefined,
    onSubpageChange: (subpage: string) => void,
  ) {
    const procsWithMemResult = this.slot.use({
      key: '',
      compute: () => loadProcessMemoryStats(trace.engine),
    });

    const procs = procsWithMemResult.data;
    if (!procs) {
      return m(EmptyState, {icon: 'hourglass', title: 'Loading processes...'});
    }

    const bestProc = pickBestProc(procs);
    if (!bestProc) {
      return m(EmptyState, 'No processes with memory in this trace');
    }

    // Use the upid in the url bar otherwise pick the 'best' proc - the one most
    // likely to be what the user was tracing.
    const selectedUpid = subpage
      ? parseUpidFromSubpage(subpage)
      : bestProc.upid;

    return [
      m('.pf-memscope-process-select', [
        m('span.pf-memscope-process-select__label', 'Process'),
        m(
          Select,
          {
            value: selectedUpid?.toString(),
            onchange: (e: Event) => {
              assertIsInstance(e.target, HTMLSelectElement);
              onSubpageChange(e.target.value);
            },
          },
          procs.map((p) =>
            m('option', {value: p.upid.toString()}, procOptionLabel(p)),
          ),
        ),
      ]),
      Number.isNaN(selectedUpid)
        ? m('', `Unable to parse upid from url '${subpage}'`)
        : m(ProcessMemDetails, {trace, upid: selectedUpid}),
    ];
  }
}

// Returns a list processes that have memory dumps/smaps/profiles in the trace.
async function loadProcessMemoryStats(engine: Engine): Promise<ProcWithMem> {
  const result = await engine.query(`
    SELECT
      p.upid,
      p.pid,
      COALESCE(p.cmdline, p.name, '<unknown>') AS procName,
      (
        SELECT count(*)
        FROM heap_graph g
        WHERE g.upid = p.upid
      ) AS heapDumps,
      (
        SELECT count(DISTINCT ts)
        FROM profiler_smaps s
        WHERE s.upid = p.upid
      ) AS smapsSnapshots,
      (
        SELECT count(DISTINCT ts)
        FROM heap_profile_allocation a
        WHERE a.upid = p.upid
      ) AS nativeDumps
    FROM process p
    WHERE heapDumps > 0 OR smapsSnapshots > 0 OR nativeDumps > 0
    ORDER BY p.upid;
  `);
  return materializeRows(result, {
    upid: NUM,
    pid: NUM,
    procName: STR,
    heapDumps: NUM,
    smapsSnapshots: NUM,
    nativeDumps: NUM,
  });
}

// Scores a process to determine how relevant it is for the landing page.
// Higher score = more relevant. We weight by data type and count to pick
// the process with the richest memory analysis data.
function scoreProc(p: ProcMemStat): number {
  // Heap dumps are the richest data source, followed by smaps, then profiles.
  return p.heapDumps * 3 + p.smapsSnapshots * 2 + p.nativeDumps * 1;
}

function pickBestProc(procs: ProcWithMem) {
  if (procs.length === 0) return undefined;
  return procs.reduce((best, p) => (scoreProc(p) > scoreProc(best) ? p : best));
}

// The subpage might look like '/123' or even '/123/foo'
function parseUpidFromSubpage(subpage: string): number {
  const parts = subpage.split('/').filter((x) => x !== '');
  if (parts.length === 0) return Number.NaN;
  return parseInt(parts[0]);
}

function procOptionLabel(p: ProcMemStat): string {
  const parts: string[] = [];
  if (p.heapDumps > 0) parts.push(`${p.heapDumps} java_hprof`);
  if (p.nativeDumps > 0) parts.push(`${p.nativeDumps} heapprofd`);
  if (p.smapsSnapshots > 0) parts.push(`${p.smapsSnapshots} smaps`);
  return parts.length > 0 ? `${p.procName} (${parts.join(', ')})` : p.procName;
}
