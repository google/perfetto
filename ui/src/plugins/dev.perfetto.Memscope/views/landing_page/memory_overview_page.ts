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
import {Gate} from '../../../../base/mithril_utils';
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
import {
  type CaptureInfo,
  loadCaptureInfo,
  type MemoryScope,
  ProcessMemDetails,
  type SourceFacts,
} from './proc_mem_overview';
import {
  loadProcessMemoryStats,
  type ProcMemStat,
  type ProcWithMem,
} from './proc_mem_stats';
import type {MemSelection} from './selection';
import {SmapsDetail} from './smaps_detail';
import './memory_overview_page.scss';

export interface MemoryOverviewPageAttrs {
  readonly trace: Trace;
  readonly upid?: number;
  readonly scope: MemoryScope;
  readonly hdeSession?: HeapDumpExplorerSession;
  readonly hdeSubpage?: string;
  readonly autoNavigated: boolean;
  readonly openByDefault: Setting<boolean>;
  readonly hideDefaultChangedHint: Setting<boolean>;
  readonly onUpidChange: (upid: number) => void;
  readonly onScopeChange: (scope: MemoryScope, preferredTs?: time) => void;
  readonly onOpenJavaHeap: (
    ts: time,
    view: NavView,
    params?: Record<string, unknown>,
  ) => void;
}

const MAX_INLINE_DUMP_CHIPS = 4;

function formatSourceFacts(f: SourceFacts): string | undefined {
  if (f.samples === 0) return undefined;
  return (
    `${f.samples} ${f.samples === 1 ? 'sample' : 'samples'}` +
    (f.spanS !== undefined ? ` · ${f.spanS.toFixed(1)}s` : '')
  );
}

function formatOffsetSec(trace: Trace, ts: time): string {
  const secs = Number(Time.diff(ts, trace.traceInfo.start)) / 1e9;
  return `${secs.toFixed(1)}s`;
}

export class MemoryOverviewPage implements m.ClassComponent<MemoryOverviewPageAttrs> {
  private readonly procsSlot = new AsyncMemo<ProcWithMem>();
  private readonly captureSlot = new AsyncMemo<CaptureInfo>();
  private selectionByUpid = new Map<number, MemSelection>();

  onremove() {
    this.procsSlot.dispose();
    this.captureSlot.dispose();
  }

  view({attrs}: m.Vnode<MemoryOverviewPageAttrs>) {
    const {
      trace,
      upid,
      scope,
      hdeSession,
      hdeSubpage,
      autoNavigated,
      openByDefault,
      hideDefaultChangedHint,
      onUpidChange,
      onScopeChange,
      onOpenJavaHeap,
    } = attrs;

    const procs = this.procsSlot.use({
      key: trace.traceInfo.uuid,
      compute: () => loadProcessMemoryStats(trace.engine),
    }).data;

    if (procs === undefined) {
      return m(
        '.pf-memscope-page.pf-memscope-unified',
        m(
          '.pf-memscope-unified__empty',
          m(EmptyState, {icon: 'hourglass', title: 'Loading processes...'}),
        ),
      );
    }

    if (procs.length === 0) {
      return m(
        '.pf-memscope-page.pf-memscope-unified',
        m(
          '.pf-memscope-unified__empty',
          m(EmptyState, {
            icon: 'memory',
            title: 'No processes with memory data in this trace',
          }),
        ),
      );
    }

    const selectedUpid = upid;
    const validUpid =
      selectedUpid !== undefined && !Number.isNaN(selectedUpid)
        ? selectedUpid
        : undefined;
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
    const dumpsCount =
      procDumps.length > 0
        ? procDumps.length
        : (capture?.dumps ?? selectedProc?.heapDumps);

    const smapsAvailable = smapsCount === undefined || smapsCount > 0;
    const heapAvailable = dumpsCount === undefined || dumpsCount > 0;

    // Resolve effective scope: if the URL asks for a scope this process has no
    // data for (once loaded), keep heap renderable as an empty state if
    // directly navigated or fall back gracefully for smaps.
    const effectiveScope: MemoryScope =
      scope === 'smaps' && !smapsAvailable ? 'overview' : scope;

    const currentSelection =
      validUpid !== undefined ? this.selectionByUpid.get(validUpid) : undefined;

    return m('.pf-memscope-page.pf-memscope-unified', [
      this.renderUnifiedHeader({
        trace,
        procs,
        selectedProc,
        validUpid,
        capture,
        scope: effectiveScope,
        smapsCount,
        dumpsCount,
        smapsAvailable,
        heapAvailable,
        procDumps,
        hdeSession,
        selection: currentSelection,
        onClearSelection:
          validUpid !== undefined
            ? () => this.selectionByUpid.delete(validUpid)
            : undefined,
        onUpidChange,
        onScopeChange,
      }),
      m('.pf-memscope-unified__body', [
        captureError !== undefined &&
          m(
            '.pf-memscope-scroll-canvas',
            m(
              '.pf-memscope-page__content',
              m('p.pf-error', `Error: ${captureError}`),
            ),
          ),
        validUpid === undefined &&
          m(
            '.pf-memscope-unified__empty',
            m(EmptyState, {
              icon: 'error_outline',
              title: 'Unable to resolve process from URL',
            }),
          ),
        validUpid !== undefined &&
          captureError === undefined && [
            m(
              Gate,
              {open: effectiveScope === 'overview'},
              m(
                '.pf-memscope-scroll-canvas',
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
                      ? () => onScopeChange('smaps')
                      : undefined,
                    onOpenJavaHeap: heapAvailable ? onOpenJavaHeap : undefined,
                  }),
                ]),
              ),
            ),
            m(
              Gate,
              {open: effectiveScope === 'smaps'},
              m(
                '.pf-memscope-scroll-canvas',
                m(
                  '.pf-memscope-page__content',
                  m(SmapsDetail, {trace, upid: validUpid}),
                ),
              ),
            ),
            m(
              Gate,
              {open: effectiveScope === 'heap'},
              this.renderHeapScope({
                trace,
                procs,
                validUpid,
                procDumps,
                hdeSession,
                hdeSubpage,
                active: effectiveScope === 'heap',
                onUpidChange,
                onScopeChange,
              }),
            ),
          ],
      ]),
    ]);
  }

  private renderUnifiedHeader(opts: {
    readonly trace: Trace;
    readonly procs: ProcWithMem;
    readonly selectedProc?: ProcMemStat;
    readonly validUpid?: number;
    readonly capture?: CaptureInfo;
    readonly scope: MemoryScope;
    readonly smapsCount?: number;
    readonly dumpsCount?: number;
    readonly smapsAvailable: boolean;
    readonly heapAvailable: boolean;
    readonly procDumps: ReadonlyArray<HeapDump>;
    readonly hdeSession?: HeapDumpExplorerSession;
    readonly selection?: MemSelection;
    readonly onClearSelection?: () => void;
    readonly onUpidChange: (upid: number) => void;
    readonly onScopeChange: (scope: MemoryScope, preferredTs?: time) => void;
  }): m.Children {
    const {
      trace,
      procs,
      selectedProc,
      validUpid,
      capture,
      scope,
      smapsCount,
      dumpsCount,
      smapsAvailable,
      heapAvailable,
      procDumps,
      hdeSession,
      selection,
      onClearSelection,
      onUpidChange,
      onScopeChange,
    } = opts;

    const durationS = Number(trace.traceInfo.end - trace.traceInfo.start) / 1e9;
    const loadingCapture = capture === undefined;
    const procName = selectedProc?.procName ?? capture?.processName ?? '…';
    const pid = selectedProc?.pid ?? capture?.pid;

    const sources: ReadonlyArray<{
      readonly key: 'smaps' | 'dump' | 'native';
      readonly label: string;
      readonly facts?: string;
    }> = [
      {
        key: 'smaps',
        label: 'smaps',
        facts:
          capture !== undefined ? formatSourceFacts(capture.smaps) : undefined,
      },
      {
        key: 'dump',
        label: 'java_hprof',
        facts:
          capture !== undefined && capture.dumps > 0
            ? `${capture.dumps} ${capture.dumps === 1 ? 'dump' : 'dumps'}`
            : undefined,
      },
      {
        key: 'native',
        label: 'heapprofd',
        facts:
          capture !== undefined ? formatSourceFacts(capture.native) : undefined,
      },
    ];

    const scopeItems: ReadonlyArray<{
      readonly id: MemoryScope;
      readonly icon: string;
      readonly label: string;
      readonly meta: string;
      readonly dot?: 'smaps' | 'dump';
      readonly disabled?: boolean;
      readonly tooltip?: string;
    }> = [
      {
        id: 'overview',
        icon: 'layers',
        label: 'Process Overview',
        meta: 'Summary · Timeline · All sources',
      },
      {
        id: 'smaps',
        icon: 'table_rows',
        label: 'Smaps VMAs',
        dot: smapsAvailable ? 'smaps' : undefined,
        meta:
          smapsCount === undefined
            ? 'Virtual mappings'
            : smapsCount > 0
              ? `${smapsCount} ${smapsCount === 1 ? 'snapshot' : 'snapshots'}`
              : 'No smaps',
        disabled: !smapsAvailable,
        tooltip: !smapsAvailable
          ? 'No smaps snapshots captured for this process'
          : 'Inspect virtual memory mappings (RSS, PSS, swap, private/shared)',
      },
      {
        id: 'heap',
        icon: 'account_tree',
        label: 'Java Heap',
        dot: heapAvailable ? 'dump' : undefined,
        meta:
          dumpsCount === undefined
            ? 'Object graph · HPROF'
            : dumpsCount > 0
              ? `${dumpsCount} ${dumpsCount === 1 ? 'dump' : 'dumps'} · Objects & flamegraph`
              : 'No heap dumps',
        disabled: !heapAvailable,
        tooltip: !heapAvailable
          ? 'No Java heap dumps captured for this process'
          : 'Explore retained objects, flamegraph, dominators, bitmaps & strings',
      },
    ];

    return m('header.pf-memscope-header', [
      // Row 1: Brand + Process Picker + Live Source Telemetry + Actions
      m('.pf-memscope-header__top', [
        m('.pf-memscope-header__subject', [
          m('.pf-memscope-brand', [
            m('.pf-memscope-brand__icon', m(Icon, {icon: 'memory'})),
            m('span.pf-memscope-brand__title', 'Memory'),
            m('span.pf-memscope-brand__badge', 'preview'),
          ]),
          m('.pf-memscope-header__divider'),
          m(
            PopupMenu,
            {
              position: PopupPosition.BottomStart,
              trigger: m(
                'button.pf-memscope-proc-trigger',
                {
                  type: 'button',
                  title: `${procName} (${trace.traceInfo.traceTitle})`,
                },
                [
                  m('span.pf-memscope-proc-trigger__name', procName),
                  pid !== undefined &&
                    m('span.pf-memscope-proc-trigger__pid', `PID ${pid}`),
                  m(Icon, {
                    icon: 'arrow_drop_down',
                    className: 'pf-memscope-proc-trigger__caret',
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
        ]),
        m(
          '.pf-memscope-header__telemetry',
          {title: `${trace.traceInfo.traceTitle} · ${durationS.toFixed(1)}s`},
          sources.map((s) =>
            m(
              'span.pf-memscope-telemetry-pill',
              {
                className: classNames(
                  !loadingCapture &&
                    s.facts === undefined &&
                    'pf-memscope-telemetry-pill--empty',
                ),
              },
              [
                m(
                  `span.pf-memscope-capture__dot.pf-memscope-capture__dot--${s.key}`,
                ),
                m('span.pf-memscope-telemetry-pill__label', s.label),
                m(
                  'span.pf-memscope-telemetry-pill__facts',
                  loadingCapture ? '…' : (s.facts ?? 'none'),
                ),
              ],
            ),
          ),
        ),
        m('.pf-memscope-header__actions', [
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
              className: 'pf-memscope-header__feedback',
            },
            'Feedback',
          ),
        ]),
      ]),

      // Row 2: Scope Zoom Lens Bar + Contextual Dump / Snapshot Controls
      m('.pf-memscope-scope-bar', [
        m(
          '.pf-memscope-scope-ladder',
          {'role': 'tablist', 'aria-label': 'Memory analysis scope'},
          scopeItems.flatMap((item, idx) => {
            const isActive = item.id === scope;
            const node: m.Children[] = [];
            if (idx > 0) {
              node.push(
                m(Icon, {
                  icon: 'chevron_right',
                  className: 'pf-memscope-scope-ladder__chevron',
                }),
              );
            }
            node.push(
              m(
                'button.pf-memscope-scope-lens',
                {
                  'type': 'button',
                  'role': 'tab',
                  'aria-selected': isActive,
                  'disabled': item.disabled,
                  'title': item.tooltip,
                  'className': classNames(
                    isActive && 'pf-memscope-scope-lens--active',
                    item.disabled && 'pf-memscope-scope-lens--disabled',
                  ),
                  'onclick': () => {
                    if (item.disabled || isActive) return;
                    onScopeChange(
                      item.id,
                      item.id === 'heap' ? selection?.sel : undefined,
                    );
                  },
                },
                [
                  m(Icon, {
                    icon: item.icon,
                    className: 'pf-memscope-scope-lens__icon',
                  }),
                  m('.pf-memscope-scope-lens__text', [
                    m('span.pf-memscope-scope-lens__label', item.label),
                    m('span.pf-memscope-scope-lens__meta', [
                      item.dot !== undefined &&
                        m(
                          `span.pf-memscope-scope-lens__dot.pf-memscope-capture__dot--${item.dot}`,
                        ),
                      item.meta,
                    ]),
                  ]),
                ],
              ),
            );
            return node;
          }),
        ),
        m('.pf-memscope-scope-bar__context', [
          scope === 'heap' &&
            hdeSession !== undefined &&
            procDumps.length > 0 &&
            this.renderDumpSwitcher(trace, hdeSession, procDumps),
          scope === 'overview' &&
            selection !== undefined &&
            m('.pf-memscope-window-pill', [
              m(Icon, {icon: 'photo_camera'}),
              m(
                'span',
                selection.base !== undefined
                  ? `Comparing t=${formatOffsetSec(trace, selection.base)} → t=${formatOffsetSec(trace, selection.sel)}`
                  : `Pinned @ t=${formatOffsetSec(trace, selection.sel)}`,
              ),
              onClearSelection !== undefined &&
                m(Button, {
                  icon: 'close',
                  compact: true,
                  title: 'Reset to latest snapshot',
                  onclick: onClearSelection,
                }),
            ]),
        ]),
      ]),
    ]);
  }

  private renderDumpSwitcher(
    trace: Trace,
    hdeSession: HeapDumpExplorerSession,
    procDumps: ReadonlyArray<HeapDump>,
  ): m.Children {
    const active = hdeSession.activeDump;
    const activeIdx = active !== null ? procDumps.indexOf(active) : -1;

    if (procDumps.length === 1) {
      const only = procDumps[0];
      if (only === undefined) return null;
      return m('.pf-memscope-dump-bar', [
        m(Icon, {
          icon: 'photo_camera',
          className: 'pf-memscope-dump-bar__icon',
        }),
        m(
          'span.pf-memscope-dump-bar__single',
          `Dump #1 · t=${formatOffsetSec(trace, only.ts)}`,
        ),
      ]);
    }

    if (procDumps.length <= MAX_INLINE_DUMP_CHIPS) {
      return m('.pf-memscope-dump-bar', [
        m('span.pf-memscope-dump-bar__label', 'Dump'),
        procDumps.map((d, i) =>
          m(Chip, {
            key: `${d.upid}:${d.ts.toString()}`,
            label: `#${i + 1} · ${formatOffsetSec(trace, d.ts)}`,
            rounded: true,
            interactive: true,
            intent: d === active ? Intent.Primary : Intent.None,
            title: `Heap dump #${i + 1} at ${formatDuration(trace, Time.diff(d.ts, trace.traceInfo.start))}`,
            onclick: () => hdeSession.selectDump(d),
          }),
        ),
      ]);
    }

    const activeLabel =
      active !== null && activeIdx >= 0
        ? `Dump #${activeIdx + 1} / ${procDumps.length} · ${formatOffsetSec(trace, active.ts)}`
        : `${procDumps.length} dumps`;

    return m('.pf-memscope-dump-bar', [
      m(
        PopupMenu,
        {
          position: PopupPosition.BottomEnd,
          trigger: m(Button, {
            label: activeLabel,
            icon: 'photo_camera',
            rightIcon: 'arrow_drop_down',
            variant: ButtonVariant.Outlined,
            compact: true,
          }),
        },
        procDumps.map((d, i) => {
          const offset = Time.diff(d.ts, trace.traceInfo.start);
          return m(MenuItem, {
            label: `Dump #${i + 1} — ${formatDuration(trace, offset)}`,
            active: d === active,
            onclick: () => hdeSession.selectDump(d),
          });
        }),
      ),
    ]);
  }

  private renderHeapScope(opts: {
    readonly trace: Trace;
    readonly procs: ProcWithMem;
    readonly validUpid: number;
    readonly procDumps: ReadonlyArray<HeapDump>;
    readonly hdeSession?: HeapDumpExplorerSession;
    readonly hdeSubpage?: string;
    readonly active: boolean;
    readonly onUpidChange: (upid: number) => void;
    readonly onScopeChange: (scope: MemoryScope, preferredTs?: time) => void;
  }): m.Children {
    const {
      procs,
      procDumps,
      hdeSession,
      hdeSubpage,
      active,
      onUpidChange,
      onScopeChange,
    } = opts;

    if (hdeSession === undefined || procDumps.length === 0) {
      const otherWithHeap = procs.find((p) => p.heapDumps > 0);
      return m(
        '.pf-memscope-unified__empty',
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
            m('.pf-memscope-empty-actions', [
              m(Button, {
                label: 'Back to Process Overview',
                icon: 'layers',
                variant: ButtonVariant.Filled,
                onclick: () => onScopeChange('overview'),
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
      '.pf-memscope-heap-canvas',
      m(HeapDumpPage, {
        session: hdeSession,
        subpage: hdeSubpage,
        embedded: true,
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
          'Memory is now the default page when opening traces with memory ' +
            'snapshots or heap dumps. Use the scope bar above to zoom ' +
            'between Process Overview, Smaps VMAs, and Java Heap.',
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
  const badges: m.Children[] = [];
  if (p.smapsSnapshots > 0) {
    badges.push(
      m('span.pf-memscope-proc-option__tag', [
        m('span.pf-memscope-capture__dot.pf-memscope-capture__dot--smaps'),
        `${p.smapsSnapshots} smaps`,
      ]),
    );
  }
  if (p.heapDumps > 0) {
    badges.push(
      m('span.pf-memscope-proc-option__tag', [
        m('span.pf-memscope-capture__dot.pf-memscope-capture__dot--dump'),
        `${p.heapDumps} java`,
      ]),
    );
  }
  if (p.nativeDumps > 0) {
    badges.push(
      m('span.pf-memscope-proc-option__tag', [
        m('span.pf-memscope-capture__dot.pf-memscope-capture__dot--native'),
        `${p.nativeDumps} native`,
      ]),
    );
  }
  return m('.pf-memscope-proc-option', [
    m('.pf-memscope-proc-option__main', [
      m('span.pf-memscope-proc-option__name', p.procName),
      m('span.pf-memscope-proc-option__pid', `PID ${p.pid}`),
    ]),
    badges.length > 0 && m('.pf-memscope-proc-option__tags', badges),
  ]);
}
