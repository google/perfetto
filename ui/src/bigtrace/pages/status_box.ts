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
import {Box} from '../../widgets/box';
import {Button} from '../../widgets/button';
import {PopupPosition} from '../../widgets/popup';
import {Tooltip} from '../../widgets/tooltip';
import {Duration} from '../../base/time';
import {
  formatCompact,
  statusDisplayLabel,
  TERMINAL_STATUSES,
} from '../query/query_store';
import type {BigTraceEditorTab} from './query_tabs_state';

// "<1s" for sub-500ms runs so the user sees the query actually ran.
export function formatDurationS(ms: number): string {
  if (ms < 500) return '<1s';
  return Duration.format(Duration.fromMillis(Math.round(ms / 1000) * 1000));
}

interface RunningDurationAttrs {
  readonly duration: string;
}

// Redraws every second so the running time ticks.
class RunningDuration implements m.ClassComponent<RunningDurationAttrs> {
  private readonly timer = window.setInterval(() => m.redraw(), 1000);

  onremove(): void {
    window.clearInterval(this.timer);
  }

  view({attrs}: m.CVnode<RunningDurationAttrs>): m.Children {
    return m('span.pf-bt-status-bar-duration-value', attrs.duration);
  }
}

export function renderStatusBox(
  tab: BigTraceEditorTab,
  onRefresh?: () => void,
): m.Children {
  if (!tab.queryUuid) return false;
  if (
    !tab.isLoading &&
    (tab.execution === undefined || tab.execution.status === 'UNKNOWN')
  ) {
    return false;
  }

  const isTerminal =
    tab.execution?.status !== undefined &&
    TERMINAL_STATUSES.has(tab.execution.status);
  const processedRows = tab.execution?.processedRows ?? 0;

  let durationMs = 0;
  if (
    isTerminal &&
    tab.execution?.endTime !== undefined &&
    tab.execution?.startTime !== undefined
  ) {
    durationMs = tab.execution.endTime - tab.execution.startTime;
  } else if (!isTerminal) {
    const start =
      tab.execution?.startTime !== undefined
        ? tab.execution.startTime
        : tab.clientStartTime;
    if (start !== undefined) {
      durationMs = Date.now() - start;
    }
  }

  const status = tab.execution?.status ?? 'UNKNOWN';
  const processedTraces = tab.execution?.processedTraces ?? 0;
  const totalTraces = tab.execution?.totalTraces ?? 0;
  const buildingFullTable = tab.execution?.phase === 'FULL_TABLE';
  const durationStr = formatDurationS(durationMs);

  const leftGroup = m(
    '.pf-bt-status-bar-group',
    m(
      'div.pf-bt-status-bar-refresh',
      m(Button, {
        icon: 'refresh',
        title: 'Refresh data',
        onclick: () => onRefresh?.(),
      }),
    ),
    m(
      'span.pf-bt-status-bar-pill',
      {className: `pf-bt-status-${status.toLowerCase().replace(/_/g, '-')}`},
      statusDisplayLabel(status),
    ),
    m(
      'span.pf-bt-status-bar-duration',
      isTerminal
        ? m('span.pf-bt-status-bar-duration-value', durationStr)
        : m(RunningDuration, {duration: durationStr}),
    ),
  );

  const rowsStatClasses = [
    'pf-bt-status-bar-stat',
    'pf-bt-status-bar-stat--rows',
    processedRows === 0 && 'pf-bt-status-bar-stat--empty',
  ]
    .filter(Boolean)
    .join(' ');
  const rightGroup = isTerminal
    ? m(
        '.pf-bt-status-bar-group',
        m(
          'span.pf-bt-status-bar-stat.pf-bt-status-bar-stat--traces',
          m('span.pf-bt-status-bar-stat-label', 'Seen traces:'),
          m(
            'span.pf-bt-status-bar-stat-value',
            {
              title:
                totalTraces > 0
                  ? `${processedTraces.toLocaleString()} of ${totalTraces.toLocaleString()}`
                  : processedTraces.toLocaleString(),
            },
            formatCompact(processedTraces),
          ),
        ),
        m(
          'span',
          {className: rowsStatClasses},
          m('span.pf-bt-status-bar-stat-label', 'Rows:'),
          m(
            'span.pf-bt-status-bar-stat-value',
            {
              title: `${processedRows.toLocaleString()} of result limit ${tab.limit.toLocaleString()}`,
            },
            formatCompact(processedRows),
          ),
        ),
      )
    : renderRunningProgress(processedTraces, totalTraces, buildingFullTable);

  return m(Box, {className: 'pf-bt-status-bar'}, leftGroup, rightGroup);
}

function renderRunningProgress(
  processedTraces: number,
  totalTraces: number,
  buildingFullTable: boolean,
): m.Children {
  const stageLabel = buildingFullTable
    ? 'Building full table…'
    : 'Preparing preview…';
  const tooltipText =
    totalTraces > 0
      ? `Seen traces: ${formatCompact(processedTraces)} / ${formatCompact(totalTraces)}`
      : processedTraces > 0
        ? `Seen traces: ${formatCompact(processedTraces)}`
        : stageLabel.replace(/…$/, '');
  return m(
    Tooltip,
    {
      trigger: m(
        '.pf-bt-status-bar-group',
        m(
          'span.pf-bt-status-bar-progress',
          m('span.pf-bt-status-bar-stage', stageLabel),
          renderInlineProgressBar(processedTraces, totalTraces, stageLabel),
        ),
      ),
      position: PopupPosition.Top,
    },
    m('.pf-bt-status-bar-progress-tooltip', m('div', tooltipText)),
  );
}

function renderInlineProgressBar(
  done: number,
  total: number,
  label: string,
): m.Children {
  if (total <= 0) {
    return m(
      'span.pf-bt-inline-progress.pf-bt-inline-progress--indeterminate',
      {
        'role': 'progressbar',
        'aria-label': label,
      },
      m('span.pf-bt-inline-progress-fill'),
    );
  }
  const pct = Math.max(0, Math.min(100, (done / total) * 100));
  return m(
    'span.pf-bt-inline-progress',
    {
      'role': 'progressbar',
      'aria-label': label,
      'aria-valuemin': 0,
      'aria-valuemax': total,
      'aria-valuenow': Math.max(0, Math.min(done, total)),
    },
    m('span.pf-bt-inline-progress-fill', {
      style: {width: `${pct}%`},
    }),
  );
}
