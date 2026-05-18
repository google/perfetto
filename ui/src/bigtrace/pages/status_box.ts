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
import {BigtraceAsyncDataSource} from '../query/bigtrace_async_data_source';
import {
  formatCompact,
  queryStore,
  statusDisplayLabel,
  TERMINAL_STATUSES,
} from '../query/query_store';
import type {BigTraceEditorTab} from './query_tabs_state';

// "<1s" for sub-500ms runs so the user sees the query actually ran.
export function formatDurationS(ms: number): string {
  if (ms < 500) return '<1s';
  return Duration.format(Duration.fromMillis(Math.round(ms / 1000) * 1000));
}

export function renderStatusBox(tab: BigTraceEditorTab): m.Children {
  if (!tab.materialize || !tab.queryUuid) return false;

  const isTerminal =
    tab.execution?.status !== undefined &&
    TERMINAL_STATUSES.has(tab.execution.status);
  const processedRows = tab.execution?.processedRows ?? 0;
  const hasNewData = !isTerminal && processedRows > tab.lastProcessedRows;

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
  const durationStr = formatDurationS(durationMs);

  const leftGroup = m(
    '.pf-query-page__status-bar-group',
    m(
      'div.pf-query-page__status-bar-refresh',
      m(Button, {
        icon: 'refresh',
        title: hasNewData
          ? 'New data available. Click to refresh.'
          : 'Refresh data',
        onclick: () => refreshAsyncStatus(tab),
      }),
      hasNewData &&
        m('span.pf-query-page__status-bar-notif', {
          'aria-label': 'New data available',
        }),
    ),
    m(
      'span.pf-query-page__status-bar-pill',
      {className: `pf-status-${status.toLowerCase().replace(/_/g, '-')}`},
      statusDisplayLabel(status),
    ),
    m(
      'span.pf-query-page__status-bar-duration',
      m('span.pf-query-page__status-bar-duration-value', durationStr),
    ),
  );

  const rowsStatClasses = [
    'pf-query-page__status-bar-stat',
    'pf-query-page__status-bar-stat--rows',
    processedRows === 0 && 'pf-query-page__status-bar-stat--empty',
  ]
    .filter(Boolean)
    .join(' ');
  const rightGroupContent = m(
    '.pf-query-page__status-bar-group',
    m(
      'span.pf-query-page__status-bar-stat.pf-query-page__status-bar-stat--traces',
      m('span.pf-query-page__status-bar-stat-label', 'Traces:'),
      m(
        'span.pf-query-page__status-bar-stat-value',
        {
          title:
            `${processedTraces.toLocaleString()} of ` +
            `${totalTraces.toLocaleString()}` +
            (!isTerminal
              ? ' — numerator lags the poll (≤3s); denominator is exact.'
              : ''),
        },
        formatCompact(processedTraces),
      ),
      renderInlineProgressBar(processedTraces, totalTraces, !isTerminal),
    ),
    m(
      'span',
      {className: rowsStatClasses},
      m('span.pf-query-page__status-bar-stat-label', 'Rows:'),
      m(
        'span.pf-query-page__status-bar-stat-value',
        {
          title: `${processedRows.toLocaleString()} of result limit ${tab.limit.toLocaleString()}`,
        },
        formatCompact(processedRows),
      ),
      renderInlineProgressBar(processedRows, tab.limit, !isTerminal),
    ),
  );

  const rightGroup = !isTerminal
    ? m(
        Tooltip,
        {
          trigger: rightGroupContent,
          position: PopupPosition.Top,
        },
        m(
          '.pf-query-page__status-bar-progress-tooltip',
          m('div', `Traces: ${formatCompact(processedTraces)}`),
          m('div', `Rows: ${formatCompact(processedRows)}`),
        ),
      )
    : rightGroupContent;

  return m(
    Box,
    {
      className: isTerminal
        ? 'pf-query-page__status-bar'
        : 'pf-query-page__status-bar pf-query-page__status-bar--running',
    },
    leftGroup,
    rightGroup,
  );
}

function renderInlineProgressBar(
  done: number,
  total: number,
  live: boolean,
): m.Children {
  if (!live) return null;
  if (total <= 0) return null;
  const pct = Math.max(0, Math.min(100, (done / total) * 100));
  return m(
    'span.pf-query-page__inline-progress',
    m('span.pf-query-page__inline-progress-fill', {
      style: {width: `${pct}%`},
    }),
  );
}

async function refreshAsyncStatus(tab: BigTraceEditorTab): Promise<void> {
  if (!tab.queryUuid) return;
  try {
    const status = await tab.queryClient?.getStatus(
      tab.queryUuid,
      tab.lifecycle.signal,
    );
    if (status) {
      queryStore.update(tab.queryUuid, {
        processedRows: status.processedRows ?? 0,
        processedTraces: status.processedTraces ?? 0,
        totalTraces: status.totalTraces ?? 0,
        status: status.status ?? 'N/A',
      });
    }
  } catch (e) {
    console.error('Failed to fetch query status on refresh:', e);
  }
  if (tab.dataSource instanceof BigtraceAsyncDataSource) {
    tab.dataSource.refresh();
    tab.lastProcessedRows = tab.execution?.processedRows ?? 0;
  }
  m.redraw();
}
