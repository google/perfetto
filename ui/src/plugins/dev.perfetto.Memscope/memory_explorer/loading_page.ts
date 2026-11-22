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
import {formatBytesIec} from '../../../base/bytes_format';
import {classNames} from '../../../base/classnames';
import {clamp} from '../../../base/math_utils';
import type {App, TraceLoadPhase, TraceLoadProgress} from '../../../public/app';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Intent} from '../../../widgets/common';
import {Icon} from '../../../widgets/icon';
import {Spinner} from '../../../widgets/spinner';
import {memscopeLiveHref} from '../process_focus';
import {
  clearPendingMemoryCapture,
  getPendingMemoryCapture,
  type PendingMemoryCapture,
} from './loading_state';
import './styles.scss';

type StepStatus = 'done' | 'active' | 'pending';

interface LoadingStep {
  readonly key: string;
  readonly label: string;
  readonly detail?: string;
  readonly status: StepStatus;
}

const PHASE_ORDER: Record<TraceLoadPhase, number> = {
  downloading: 0,
  initializing: 1,
  parsing: 1,
  indexing: 2,
  plugins: 3,
  finalizing: 3,
};

function fmtTransfer(
  bytesRead?: number,
  bytesTotal?: number,
  mbPerSec?: number,
): string | undefined {
  const parts: string[] = [];
  if (
    bytesRead !== undefined &&
    bytesRead > 0 &&
    bytesTotal !== undefined &&
    bytesTotal > 0
  ) {
    parts.push(`${formatBytesIec(bytesRead)} / ${formatBytesIec(bytesTotal)}`);
  } else if (bytesRead !== undefined && bytesRead > 0) {
    parts.push(formatBytesIec(bytesRead));
  }
  if (mbPerSec !== undefined && mbPerSec > 0) parts.push(`${mbPerSec} MB/s`);
  return parts.length > 0 ? parts.join(' · ') : undefined;
}

function buildSteps(
  capture: PendingMemoryCapture | undefined,
  progress: TraceLoadProgress | undefined,
  preparingExplorer = false,
): LoadingStep[] {
  const steps: LoadingStep[] = [];
  const stopping = capture?.stage === 'stopping' && progress === undefined;

  if (capture?.fromLive) {
    steps.push({
      key: 'capture',
      label: 'Capture from device',
      detail: stopping
        ? capture.detail
        : capture.capturedBytes !== undefined
          ? `Captured ${formatBytesIec(capture.capturedBytes)}`
          : 'Captured',
      status: stopping ? 'active' : 'done',
    });
  }

  const order = preparingExplorer
    ? 4
    : progress !== undefined
      ? PHASE_ORDER[progress.phase]
      : -1;
  const parseStatus: StepStatus =
    order > 1 ? 'done' : order >= 0 ? 'active' : 'pending';
  const indexStatus: StepStatus =
    order > 2 ? 'done' : order === 2 ? 'active' : 'pending';
  const explorerStatus: StepStatus = order >= 3 ? 'active' : 'pending';
  const doneBytes =
    progress?.bytesTotal ?? progress?.bytesRead ?? capture?.capturedBytes;

  steps.push(
    {
      key: 'parse',
      label:
        progress?.phase === 'downloading'
          ? 'Download & parse trace'
          : 'Read & parse trace',
      detail:
        parseStatus === 'active'
          ? (fmtTransfer(
              progress?.bytesRead,
              progress?.bytesTotal,
              progress?.mbPerSec,
            ) ?? progress?.status)
          : parseStatus === 'done' && doneBytes !== undefined
            ? formatBytesIec(doneBytes)
            : undefined,
      status: parseStatus,
    },
    {
      key: 'index',
      label: 'Index memory & heap graphs',
      detail:
        indexStatus === 'active'
          ? 'Building SQL tables, smaps & dominator trees…'
          : undefined,
      status: indexStatus,
    },
    {
      key: 'explorer',
      label: 'Prepare Memory Explorer',
      detail: preparingExplorer
        ? 'Loading process memory summary…'
        : explorerStatus === 'active'
          ? progress?.status
          : undefined,
      status: explorerStatus,
    },
  );
  return steps;
}

function renderLoadingShell(opts: {
  readonly title: string;
  readonly processName?: string;
  readonly pid?: number;
  readonly status: string;
  readonly pct?: number;
  readonly bytesRead?: number;
  readonly bytesTotal?: number;
  readonly mbPerSec?: number;
  readonly steps: ReadonlyArray<LoadingStep>;
  readonly fromLive?: boolean;
  readonly error?: string;
  readonly onBackToLive?: () => void;
  readonly onOpenTrace?: () => void;
}): m.Children {
  const clampedPct =
    opts.pct !== undefined ? clamp(Math.round(opts.pct), 0, 100) : undefined;
  const sizeBytes =
    opts.bytesTotal !== undefined && opts.bytesTotal > 0
      ? opts.bytesTotal
      : opts.bytesRead;
  const throughput = fmtTransfer(
    opts.bytesRead,
    opts.bytesTotal,
    opts.mbPerSec,
  );

  return m('.pf-hde-page', [
    m(
      '.pf-memory-explorer__header',
      m('.pf-memscope-load-header', [
        m('span.pf-memscope-load-header__title', 'MEMORY EXPLORER'),
        opts.processName && m('span.pf-memscope-load__pill', opts.processName),
        opts.pid !== undefined &&
          m('span.pf-memscope-load__pill', `PID ${opts.pid}`),
      ]),
    ),
    m(
      '.pf-memscope-load',
      m('.pf-memscope-load__card', [
        m('.pf-memscope-load__head', [
          m(
            '.pf-memscope-load__icon',
            {
              className: classNames(
                opts.error && 'pf-memscope-load__icon--error',
              ),
            },
            m(Icon, {icon: opts.error ? 'error' : 'memory'}),
          ),
          m('.pf-memscope-load__titles', [
            m(
              'h2.pf-memscope-load__title',
              opts.error
                ? 'Could not open memory capture'
                : opts.fromLive
                  ? 'Opening memory capture'
                  : 'Loading memory trace',
            ),
            m('.pf-memscope-load__meta', [
              m('span.pf-memscope-load__filename', opts.title),
              opts.processName &&
                m('span.pf-memscope-load__pill', opts.processName),
              opts.pid !== undefined &&
                m('span.pf-memscope-load__pill', `PID ${opts.pid}`),
              sizeBytes !== undefined && sizeBytes > 0
                ? m('span.pf-memscope-load__pill', formatBytesIec(sizeBytes))
                : undefined,
            ]),
          ]),
        ]),
        opts.error !== undefined
          ? [
              m('.pf-memscope-load__error', opts.error),
              m('.pf-memscope-load__actions', [
                opts.onBackToLive &&
                  m(Button, {
                    label: 'Back to Live',
                    icon: 'arrow_back',
                    intent: Intent.Primary,
                    variant: ButtonVariant.Filled,
                    onclick: opts.onBackToLive,
                  }),
                opts.onOpenTrace &&
                  m(Button, {
                    label: 'Open another trace',
                    icon: 'folder_open',
                    variant: ButtonVariant.Outlined,
                    onclick: opts.onOpenTrace,
                  }),
              ]),
            ]
          : [
              m('.pf-memscope-load__bar-block', [
                m('.pf-memscope-load__bar-row', [
                  m('span.pf-memscope-load__status', opts.status),
                  m(
                    'span.pf-memscope-load__pct',
                    clampedPct !== undefined ? `${clampedPct}%` : 'Working…',
                  ),
                ]),
                m(
                  '.pf-memscope-load__track',
                  clampedPct !== undefined
                    ? m('.pf-memscope-load__fill', {
                        style: {width: `${Math.max(clampedPct, 2)}%`},
                      })
                    : m(
                        '.pf-memscope-load__fill.pf-memscope-load__fill--indeterminate',
                      ),
                ),
                throughput && m('.pf-memscope-load__throughput', throughput),
              ]),
              m(
                'ol.pf-memscope-load__steps',
                opts.steps.map((step) =>
                  m(
                    'li.pf-memscope-load__step',
                    {
                      key: step.key,
                      className: `pf-memscope-load__step--${step.status}`,
                    },
                    [
                      m(
                        'span.pf-memscope-load__step-icon',
                        step.status === 'done'
                          ? m(Icon, {icon: 'check_circle'})
                          : step.status === 'active'
                            ? m(Spinner, {easing: true})
                            : m('span.pf-memscope-load__step-dot'),
                      ),
                      m('span.pf-memscope-load__step-body', [
                        m('span.pf-memscope-load__step-label', step.label),
                        step.detail &&
                          m('span.pf-memscope-load__step-detail', step.detail),
                      ]),
                    ],
                  ),
                ),
              ),
              opts.fromLive &&
                m('.pf-memscope-load__foot', [
                  m('span', 'Live polling is paused while this capture loads.'),
                  opts.onBackToLive &&
                    m(Button, {
                      label: 'Back to Live',
                      icon: 'sensors',
                      variant: ButtonVariant.Minimal,
                      compact: true,
                      onclick: opts.onBackToLive,
                    }),
                ]),
            ],
      ]),
    ),
  ]);
}

export interface MemoryLoadingPageAttrs {
  readonly app: App;
  readonly focusProcess?: string;
}

export class MemoryLoadingPage implements m.ClassComponent<MemoryLoadingPageAttrs> {
  view({
    attrs: {app, focusProcess},
  }: m.CVnode<MemoryLoadingPageAttrs>): m.Children {
    const capture = getPendingMemoryCapture();
    const progress = app.traceLoadProgress;
    const isError = capture?.stage === 'error' && !app.isLoadingTrace;
    return renderLoadingShell({
      title: progress?.title ?? capture?.title ?? 'Memory trace',
      processName: capture?.processName ?? focusProcess,
      pid: capture?.pid,
      status:
        progress?.status ?? capture?.detail ?? 'Preparing Trace Processor…',
      pct: progress?.pct ?? (capture?.stage === 'opening' ? 4 : undefined),
      bytesRead: progress?.bytesRead,
      bytesTotal: progress?.bytesTotal ?? capture?.capturedBytes,
      mbPerSec: progress?.mbPerSec,
      steps: buildSteps(capture, progress, false),
      fromLive: capture?.fromLive,
      error: isError ? capture?.error : undefined,
      onBackToLive: () => {
        if (isError) clearPendingMemoryCapture();
        app.navigate(memscopeLiveHref());
      },
      onOpenTrace: () => {
        if (isError) clearPendingMemoryCapture();
        app.commands.runCommand('dev.perfetto.OpenTrace');
      },
    });
  }
}

export function renderExplorerPreparingView(
  title: string,
  processName?: string,
): m.Children {
  return renderLoadingShell({
    title,
    processName,
    status: 'Loading process memory summary…',
    pct: 98,
    steps: buildSteps(undefined, undefined, true),
  });
}
