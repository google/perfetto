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

// Baseline-pool controls inlined into the top bar. Renders nothing when
// no baseline is loaded and no load is in flight — the "Diff against
// another trace" entry point lives in Overview in that case.

import m from 'mithril';
import {Trace} from '../../public/trace';
import {Button, ButtonVariant} from '../../widgets/button';
import {Callout} from '../../widgets/callout';
import {Intent} from '../../widgets/common';
import {RadioGroup} from '../../widgets/radio_group';
import {MenuDivider, MenuItem, PopupMenu} from '../../widgets/menu';
import type {DiffMode, BaselineDumpRef, BaselineTrace} from './baseline/state';
import {
  clearActiveBaseline,
  dispose as disposeAllBaselines,
  getActiveBaseline,
  getBaselineTraces,
  getMode,
  removeBaselineTrace,
  setActiveBaseline,
  setMode,
} from './baseline/state';
import {
  clearLoadError,
  getLoadState,
  triggerFileLoad,
} from './baseline/load_action';
import type {HeapDump} from './queries';

const MODES: ReadonlyArray<{key: DiffMode; label: string}> = [
  {key: 'diff', label: 'Diff'},
  {key: 'current', label: 'Current'},
  {key: 'baseline', label: 'Baseline'},
];

const FILE_ACCEPT =
  '.pftrace,.hprof,.perfetto-trace,.pb,.gz,application/octet-stream';

/**
 * True when the baseline header should contribute UI to the top bar. The
 * parent uses this to decide whether to render the row at all in
 * combination with the primary dump selector.
 */
export function shouldShowBaselineHeader(): boolean {
  const traces = getBaselineTraces();
  const {loading, error} = getLoadState();
  const hasError = error !== null && getActiveBaseline() === null;
  return traces.length > 0 || loading || hasError;
}

/**
 * Triggers the hidden file input owned by the most recently mounted
 * baseline header instance, if any. Lets the Overview-tab CTA share the
 * same picker as the top-bar selector — no parallel hidden inputs that
 * need separate state to coordinate.
 */
let openFilePickerImpl: (() => void) | null = null;
export function openBaselineFilePicker(): void {
  if (openFilePickerImpl) {
    openFilePickerImpl();
  } else {
    requestAnimationFrame(() => openFilePickerImpl?.());
  }
}

interface HeapDumpDiffHeaderAttrs {
  readonly trace: Trace;
}

export class HeapDumpDiffHeader
  implements m.ClassComponent<HeapDumpDiffHeaderAttrs>
{
  private inputEl: HTMLInputElement | null = null;

  oncreate() {
    openFilePickerImpl = () => this.inputEl?.click();
  }
  onremove() {
    openFilePickerImpl = null;
  }
  view({attrs}: m.Vnode<HeapDumpDiffHeaderAttrs>): m.Children {
    const traces = getBaselineTraces();
    const active = getActiveBaseline();
    const {loading, progressPct, error} = getLoadState();
    const hidden = !shouldShowBaselineHeader();

    // Always render the hidden input so the Overview CTA can click it.
    const fileInput = m('input', {
      'type': 'file',
      'accept': FILE_ACCEPT,
      'style': 'display:none',
      'aria-hidden': 'true',
      'oncreate': (v) => {
        this.inputEl = v.dom as HTMLInputElement;
      },
      'onchange': async (e: Event) => {
        const file = (e.target as HTMLInputElement).files?.[0];
        if (!file) return;
        await triggerFileLoad(attrs.trace.raf, file);
        if (this.inputEl) this.inputEl.value = '';
      },
    });

    if (hidden) return fileInput;

    return [
      fileInput,
      loading ? renderLoadingCallout(progressPct) : null,
      error && !active ? renderErrorCallout(error) : null,
      m('span', {class: 'ah-top-bar__label'}, 'Baseline:'),
      renderBaselineSelector(traces, active, () => this.inputEl?.click()),
      m('span', {class: 'ah-top-bar__spacer'}),
      active
        ? m(
            RadioGroup,
            {
              'selectedValue': getMode(),
              'onValueChange': (value: string) => setMode(value as DiffMode),
              'aria-label': 'Diff mode',
            },
            MODES.map((mode) =>
              m(RadioGroup.Button, {value: mode.key}, mode.label),
            ),
          )
        : null,
      // One-click affordances: mirror the popup MenuItems but exposed
      // directly in the top-bar so users can deactivate or tear down
      // baselines without re-opening the picker. The popup keeps the
      // same actions as the canonical entry point.
      active
        ? m(Button, {
            'icon': 'close',
            'compact': true,
            'aria-label': 'Clear active baseline',
            'title': 'Clear active baseline',
            'onclick': () => clearActiveBaseline(),
          })
        : null,
      traces.length > 0
        ? m(Button, {
            'icon': 'delete_sweep',
            'compact': true,
            'aria-label': 'Remove all baseline traces',
            'title': 'Remove all baseline traces',
            'onclick': () => disposeAllBaselines(),
          })
        : null,
    ];
  }
}

function renderLoadingCallout(progressPct: number): m.Children {
  return m(
    Callout,
    {icon: 'hourglass_empty', intent: Intent.None},
    `Loading baseline trace… ${progressPct}%`,
  );
}

function renderErrorCallout(message: string): m.Children {
  return m(
    Callout,
    {
      'icon': 'error',
      'intent': Intent.Danger,
      'dismissible': true,
      'onDismiss': () => clearLoadError(),
      'role': 'alert',
      'aria-live': 'assertive',
    },
    message,
  );
}

function renderBaselineSelector(
  traces: ReadonlyArray<BaselineTrace>,
  active: BaselineDumpRef | null,
  openFilePicker: () => void,
): m.Children {
  const triggerLabel = active ? activeLabel(active) : 'None — pick to diff';
  return m(
    PopupMenu,
    {
      trigger: m(Button, {
        label: triggerLabel,
        icon: 'difference',
        rightIcon: 'arrow_drop_down',
        variant: ButtonVariant.Outlined,
        compact: true,
      }),
    },
    [
      ...traces.flatMap((t) => renderTraceSection(t, active)),
      traces.length > 0 ? m(MenuDivider) : null,
      m(MenuItem, {
        label: 'Add baseline trace…',
        icon: 'upload_file',
        onclick: openFilePicker,
      }),
      active
        ? m(MenuItem, {
            label: 'Clear active baseline',
            icon: 'close',
            onclick: () => clearActiveBaseline(),
          })
        : null,
    ],
  );
}

function renderTraceSection(
  t: BaselineTrace,
  active: BaselineDumpRef | null,
): m.Children[] {
  const heading = m(MenuItem, {
    label: m('span', {class: 'ah-top-bar__section-title'}, t.title),
    icon: 'folder_open',
    closePopupOnClick: false,
    onclick: () => {},
  });
  const dumpItems = t.dumps.map((d) =>
    m(MenuItem, {
      label: dumpLabel(d, t.dumps),
      icon: active && active.trace === t && active.dump === d ? 'check' : '',
      onclick: () => setActiveBaseline({trace: t, dump: d}),
    }),
  );
  const removeItem = m(MenuItem, {
    label: `Remove ${t.title}`,
    icon: 'delete',
    onclick: () => removeBaselineTrace(t.id),
  });
  return [heading, ...dumpItems, removeItem, m(MenuDivider)];
}

function activeLabel(b: BaselineDumpRef): string {
  return `${b.trace.title} · ${dumpProcessLabel(b.dump)}`;
}

// Multi-dump traces show an offset from `dumps[0].ts`; baseline trace
// times aren't commensurable with the primary's absolute start.
function dumpLabel(d: HeapDump, dumps: ReadonlyArray<HeapDump>): string {
  if (dumps.length <= 1) return dumpProcessLabel(d);
  return `${dumpProcessLabel(d)} — ${formatBaselineOffset(d, dumps)}`;
}

function dumpProcessLabel(d: HeapDump): string {
  // hprof has no real pid — trace_processor reports 0. "pid 0" reads
  // like kernel, so treat 0 as missing.
  const hasPid = d.pid !== null && d.pid !== 0;
  if (d.processName !== null && hasPid) {
    return `${d.processName} (pid ${d.pid})`;
  }
  if (d.processName !== null) return d.processName;
  if (hasPid) return `pid ${d.pid}`;
  return 'Java heap dump';
}

// "first" / "+250ms" / "+5.2s" / "+3m 15s".
function formatBaselineOffset(
  d: HeapDump,
  dumps: ReadonlyArray<HeapDump>,
): string {
  const start = dumps[0].ts;
  const deltaNs = (d.ts as bigint) - (start as bigint);
  if (deltaNs === 0n) return 'first';
  const ms = Number(deltaNs / 1_000_000n);
  if (ms < 1000) return `+${ms}ms`;
  const sec = ms / 1000;
  if (sec < 60) return `+${sec.toFixed(1)}s`;
  const minutes = Math.floor(sec / 60);
  const remSec = Math.round(sec - minutes * 60);
  return remSec === 0 ? `+${minutes}m` : `+${minutes}m ${remSec}s`;
}
