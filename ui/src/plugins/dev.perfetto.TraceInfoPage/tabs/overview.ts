// Copyright (C) 2025 The Android Open Source Project
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
import {
  LONG_NULL,
  NUM_NULL,
  STR_NULL,
} from '../../../trace_processor/query_result';
import {Icon} from '../../../widgets/icon';
import {Tooltip} from '../../../widgets/tooltip';
import {Section} from '../../../widgets/section';
import {Card} from '../../../widgets/card';
import {GridLayout} from '../../../widgets/grid_layout';
import {EmptyState} from '../../../widgets/empty_state';
import {Trace} from '../../../public/trace';
import {duration} from '../../../base/time';
import {formatDuration} from '../../../components/time_utils';
import type {TabKey} from '../utils';
import {formatFileSize} from '../utils';

export interface OverviewData {
  // Status counts
  importErrors: number;
  traceErrors: number;
  dataLosses: number;
  uiLoadingErrorCount: number;
  // Metrics
  traceSizeBytes?: bigint;
  traceType?: string;
  traceUuid?: string;
  durationNs?: duration;
  schedDurationNs?: duration;
  // System information
  androidBuildFingerprint?: string;
  systemName?: string;
  systemMachine?: string;
  systemRelease?: string;
}

export async function loadOverviewData(trace: Trace): Promise<OverviewData> {
  // Load everything in a single query
  // Note: _metadata_str and _metadata_int functions are created in the plugin
  const result = await trace.engine.query(`
    SELECT
      -- Status card counts
      (SELECT IFNULL(sum(value), 0) FROM stats WHERE severity = 'error' AND source = 'analysis') as import_errors,
      (SELECT IFNULL(sum(value), 0) FROM stats WHERE severity = 'error' AND source = 'trace') as trace_errors,
      (SELECT IFNULL(sum(value), 0) FROM stats WHERE severity = 'data_loss') as data_losses,
      -- Metrics
      _metadata_int('trace_size_bytes') as trace_size_bytes,
      IFNULL(_metadata_str('trace_type'), 'Unknown') as trace_type,
      IFNULL(_metadata_str('trace_uuid'), 'Not available') as trace_uuid,
      _metadata_int('tracing_disabled_ns') - _metadata_int('tracing_started_ns') as duration_ns,
      (SELECT max(ts) - min(ts) FROM sched) as sched_duration_ns,
      -- System info
      _metadata_str('system_name') as system_name,
      _metadata_str('system_release') as system_release,
      _metadata_str('system_machine') as system_machine,
      _metadata_str('android_build_fingerprint') as android_build_fingerprint;
  `);

  const row = result.firstRow({
    import_errors: NUM_NULL,
    trace_errors: NUM_NULL,
    data_losses: NUM_NULL,
    trace_size_bytes: LONG_NULL,
    trace_type: STR_NULL,
    trace_uuid: STR_NULL,
    duration_ns: LONG_NULL,
    sched_duration_ns: LONG_NULL,
    system_name: STR_NULL,
    system_release: STR_NULL,
    system_machine: STR_NULL,
    android_build_fingerprint: STR_NULL,
  });
  return {
    importErrors: row.import_errors ?? 0,
    traceErrors: row.trace_errors ?? 0,
    dataLosses: row.data_losses ?? 0,
    uiLoadingErrorCount: trace.loadingErrors.length,
    traceSizeBytes: row.trace_size_bytes ?? undefined,
    traceType: row.trace_type ?? undefined,
    traceUuid: row.trace_uuid ?? undefined,
    durationNs: row.duration_ns ?? undefined,
    schedDurationNs: row.sched_duration_ns ?? undefined,
    androidBuildFingerprint: row.android_build_fingerprint ?? undefined,
    systemName: row.system_name ?? undefined,
    systemMachine: row.system_machine ?? undefined,
    systemRelease: row.system_release ?? undefined,
  };
}

export interface OverviewTabAttrs {
  trace: Trace;
  data: OverviewData;
  onTabChange(key: TabKey): void;
}

interface StatusCardConfig {
  title: string;
  count: number;
  severity: 'success' | 'danger' | 'warning';
  icon: string;
  helpText: string;
  targetTab: TabKey;
}

interface MetricCardConfig {
  label: string;
  value: string | undefined;
  help?: string;
  wide?: boolean;
}

export class OverviewTab implements m.ClassComponent<OverviewTabAttrs> {
  view({attrs}: m.CVnode<OverviewTabAttrs>) {
    return m(
      '.pf-trace-info-page__tab-content',
      this.renderCardSection(
        'Trace Health',
        'Summary of errors, warnings, and data quality indicators',
        createStatusCards(attrs.data).map((card) =>
          renderStatusCard(attrs, card),
        ),
      ),
      this.renderCardSection(
        'Trace Overview',
        'Key metadata and properties of the trace file',
        createTraceMetrics(attrs.trace, attrs.data).map((metric) =>
          renderMetricCard(metric),
        ),
      ),
      this.renderCardSection(
        'System Information',
        'Operating system and hardware details from the traced device',
        createSystemInfoMetrics(attrs.data).map((metric) =>
          renderMetricCard(metric),
        ),
        {
          emptyState: {
            icon: 'computer',
            title: 'No system information available',
          },
        },
      ),
    );
  }

  private renderCardSection(
    title: string,
    subtitle: string,
    cards: m.Children[],
    options?: {emptyState?: {icon: string; title: string}},
  ): m.Children {
    const filteredCards = cards.filter(Boolean);
    return m(
      Section,
      {title, subtitle},
      filteredCards.length === 0 && options?.emptyState
        ? m(EmptyState, options.emptyState)
        : m(GridLayout, {}, ...filteredCards),
    );
  }
}

function renderStatusCard(
  attrs: OverviewTabAttrs,
  {title, count, severity, icon, helpText, targetTab}: StatusCardConfig,
): m.Children {
  const isClickable = count > 0;
  const className = `pf-trace-info-page__status-card pf-trace-info-page__status-card--${severity}${
    isClickable ? ' pf-trace-info-page__status-card--clickable' : ''
  }`;

  return m(
    Card,
    {
      className,
      onclick: isClickable ? () => attrs.onTabChange(targetTab) : undefined,
    },
    m(
      '.pf-trace-info-page__status-card-main',
      m(Icon, {
        icon,
        className: 'pf-trace-info-page__status-icon',
        filled: true,
      }),
      m(
        '.pf-trace-info-page__status-content',
        m(
          '.pf-trace-info-page__status-title',
          title,
          m(
            Tooltip,
            {
              trigger: m(Icon, {
                icon: 'help_outline',
                className: 'pf-trace-info-page__help-icon',
              }),
            },
            helpText,
          ),
        ),
        m('.pf-trace-info-page__status-value', count),
      ),
      isClickable &&
        m(
          '.pf-trace-info-page__status-link',
          m(Icon, {
            icon: 'arrow_forward',
            className: 'pf-trace-info-page__status-link-icon',
          }),
        ),
    ),
  );
}

function renderMetricCard({
  label,
  value,
  help,
  wide,
}: MetricCardConfig): m.Children {
  if (value === undefined) {
    return null;
  }
  const className = wide
    ? 'pf-trace-info-page__metric-card pf-trace-info-page__metric-card--wide'
    : 'pf-trace-info-page__metric-card';
  return m(
    Card,
    {className},
    m(
      '.pf-trace-info-page__metric-content',
      m(
        '.pf-trace-info-page__metric-label',
        label,
        help &&
          m(
            Tooltip,
            {
              trigger: m(Icon, {
                icon: 'help_outline',
                className: 'pf-trace-info-page__help-icon',
              }),
            },
            help,
          ),
      ),
      m('.pf-trace-info-page__metric-value', value),
    ),
  );
}

function createStatusCards(data: OverviewData): StatusCardConfig[] {
  const statusCards: StatusCardConfig[] = [
    {
      title: 'Import Errors',
      count: data.importErrors,
      severity: data.importErrors === 0 ? 'success' : 'danger',
      icon: data.importErrors === 0 ? 'check_circle' : 'error',
      helpText:
        'Errors encountered during trace import by the trace processor. These may indicate missing or corrupted data.',
      targetTab: data.importErrors > 0 ? 'import_errors' : 'stats',
    },
    {
      title: 'Trace Errors',
      count: data.traceErrors,
      severity: data.traceErrors === 0 ? 'success' : 'danger',
      icon: data.traceErrors === 0 ? 'check_circle' : 'error',
      helpText:
        'Errors that occurred during trace recording. These indicate problems during data collection.',
      targetTab: data.traceErrors > 0 ? 'trace_errors' : 'stats',
    },
    {
      title: 'Data Losses',
      count: data.dataLosses,
      severity: data.dataLosses === 0 ? 'success' : 'warning',
      icon: data.dataLosses === 0 ? 'check_circle' : 'warning',
      helpText:
        'Events that were dropped during trace recording due to buffer overflow or other issues.',
      targetTab: data.dataLosses > 0 ? 'data_losses' : 'stats',
    },
  ];
  // Optional UI loading errors card - only show if there are errors
  if (data.uiLoadingErrorCount > 0) {
    statusCards.push({
      title: 'UI Loading Errors',
      count: data.uiLoadingErrorCount,
      severity: 'danger',
      icon: 'error',
      helpText:
        'Errors that occurred in the UI while loading or processing the trace.',
      targetTab: 'ui_loading_errors',
    });
  }
  return statusCards;
}

function createTraceMetrics(
  trace: Trace,
  data: OverviewData,
): MetricCardConfig[] {
  return [
    {
      label: 'Trace Size',
      value:
        data.traceSizeBytes !== undefined
          ? formatFileSize(Number(data.traceSizeBytes)).formatted
          : undefined,
      help: 'Total size of the trace file on disk',
    },
    {
      label: 'Trace Type',
      value: data.traceType,
      help: 'Format of the trace file (proto, json, etc.)',
    },
    {
      label: 'Recording Duration',
      value:
        data.durationNs !== undefined
          ? formatDuration(trace, data.durationNs)
          : undefined,
      help: 'Time between tracing_started and tracing_disabled',
    },
    {
      label: 'Scheduling Duration',
      value:
        data.schedDurationNs !== undefined
          ? formatDuration(trace, data.schedDurationNs)
          : undefined,
      help: 'Duration from first to last scheduling event (max(ts) - min(ts) from sched)',
    },
    {
      label: 'Trace UUID',
      value: data.traceUuid,
      help: 'Unique identifier for this trace session',
      wide: true,
    },
  ];
}

function createSystemInfoMetrics(data: OverviewData): MetricCardConfig[] {
  return [
    {
      label: 'Android Fingerprint',
      value: data.androidBuildFingerprint,
      help: 'Unique build identifier for Android devices',
      wide: true,
    },
    {
      label: 'Operating System',
      value: data.systemName,
      help: 'Operating system name from uname',
    },
    {
      label: 'Architecture',
      value: data.systemMachine,
      help: 'System architecture (e.g., x86_64, aarch64)',
    },
    {
      label: 'Kernel Release',
      value: data.systemRelease,
      help: 'Kernel version string',
      wide: true,
    },
  ];
}
