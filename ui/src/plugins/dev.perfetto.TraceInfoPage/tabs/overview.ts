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
import {Icon} from '../../../widgets/icon';
import {Tooltip} from '../../../widgets/tooltip';
import {Section} from '../../../widgets/section';
import {Card} from '../../../widgets/card';
import {GridLayout} from '../../../widgets/grid_layout';
import {EmptyState} from '../../../widgets/empty_state';
import {Callout} from '../../../widgets/callout';
import {Intent} from '../../../widgets/common';
import type {Trace} from '../../../public/trace';
import {Time} from '../../../base/time';
import {formatDuration} from '../../../components/time_utils';
import type {TabKey} from '../utils';
import {formatFileSize} from '../../../base/file_utils';
import {
  type Diagnostic,
  DiagnosticCard,
  dedupeDiagnosticsByKey,
} from '../diagnostics';
import {
  type OverviewData,
  type StatusCardConfig,
  createStatusCards,
} from './overview_data';

export interface OverviewTabAttrs {
  trace: Trace;
  data: OverviewData;
  diagnostics: ReadonlyArray<Diagnostic>;
  onTabChange(key: TabKey): void;
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
      m(
        Section,
        {
          title: 'Trace Doctor',
          subtitle:
            'Trace Doctor detects problems in the trace config or ' +
            'environment that can affect the quality of the trace',
        },
        attrs.diagnostics.length === 0
          ? m(EmptyState, {
              icon: 'check_circle',
              title: 'No trace config issues detected',
            })
          : m(
              GridLayout,
              // The overview summarises, so collapse the same problem across
              // traces into a single card; the Trace Doctor tab keeps them all.
              dedupeDiagnosticsByKey(attrs.diagnostics).map((diagnostic) =>
                m(DiagnosticCard, {
                  diagnostic,
                  onclick: () => attrs.onTabChange('trace_doctor'),
                }),
              ),
            ),
      ),
      this.renderCardSection(
        'Trace Overview',
        'Key metadata and properties of the trace file',
        createTraceMetrics(attrs.trace, attrs.data).map((metric) =>
          renderMetricCard(metric),
        ),
        {
          banner: attrs.data.traceCount > 1 && {
            icon: 'layers',
            text:
              'This session contains multiple traces; the values here are ' +
              'session-wide. See the "Traces" tab for per-trace details.',
          },
        },
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
          banner: attrs.data.machineCount > 1 && {
            icon: 'computer',
            text:
              'This session contains multiple machines; only the host ' +
              'machine is shown here. See the "Machines" tab for the others.',
          },
        },
      ),
    );
  }

  private renderCardSection(
    title: string,
    subtitle: string,
    cards: m.Children[],
    options?: {
      emptyState?: {icon: string; title: string};
      banner?: {icon: string; text: string} | false;
    },
  ): m.Children {
    const filteredCards = cards.filter(Boolean);
    const bannerCfg = options?.banner;
    const banner =
      bannerCfg === undefined || bannerCfg === false
        ? undefined
        : m(
            Callout,
            {
              icon: bannerCfg.icon,
              intent: Intent.Primary,
              className: 'pf-trace-info-page__banner',
            },
            bannerCfg.text,
          );
    return m(
      Section,
      {title, subtitle},
      banner,
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

function createTraceMetrics(
  trace: Trace,
  data: OverviewData,
): MetricCardConfig[] {
  return [
    {
      label: 'Trace Size',
      value:
        data.traceSizeBytes !== undefined
          ? formatFileSize(Number(data.traceSizeBytes))
          : undefined,
      help: 'Total size of the trace file on disk',
    },
    {
      label: 'Trace Type',
      value:
        data.traceTypes.length > 0 ? data.traceTypes.join(', ') : 'Unknown',
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
      label: 'Recording Started',
      value: formatTraceStartWallClock(trace),
      help: "Wall-clock time recording began, from the trace's REALTIME clock",
      wide: true,
    },
    {
      label: 'Trace UUID',
      value: data.uuid,
      help:
        data.traceCount > 1
          ? 'Session-wide identifier. Individual UUIDs are in the "Traces" tab.'
          : 'Unique identifier for this trace session',
      wide: true,
    },
  ];
}

// Renders the trace start as a wall-clock time, or undefined if the trace has
// no REALTIME clock snapshot (in which case unixOffset is zero).
function formatTraceStartWallClock(trace: Trace): string | undefined {
  const {start, unixOffset, tzOffMin} = trace.traceInfo;
  if (unixOffset === Time.ZERO) {
    return undefined;
  }
  const utc = Time.toDate(start, unixOffset);
  const local = new Date(utc.getTime() + tzOffMin * 60_000);
  const iso = local.toISOString();
  return `${iso.slice(0, 10)} ${iso.slice(11, 19)} ${formatTzOffset(tzOffMin)}`;
}

function formatTzOffset(tzOffMin: number): string {
  if (tzOffMin === 0) {
    return 'UTC';
  }
  const sign = tzOffMin > 0 ? '+' : '-';
  const abs = Math.abs(tzOffMin);
  const hh = String(Math.floor(abs / 60)).padStart(2, '0');
  const mm = String(abs % 60).padStart(2, '0');
  return `UTC${sign}${hh}:${mm}`;
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
