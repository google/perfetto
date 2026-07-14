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
import type {Engine} from '../../trace_processor/engine';
import type {Trace} from '../../public/trace';
import {
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {Card} from '../../widgets/card';
import {Section} from '../../widgets/section';
import {Icon} from '../../widgets/icon';
import {clamp} from '../../base/math_utils';
import {createStatusCards, loadOverviewData} from './tabs/overview_data';

// Diagnostics with a confidence above this are treated as high severity: they
// trigger the auto-opened details tab.
export const HIGH_CONFIDENCE_THRESHOLD = 0.5;

// A single row of the `trace_diagnostics` table: an automatically detected
// problem with the trace config or environment.
export interface Diagnostic {
  // Stable identifier (e.g. tiny_ftrace_buffer). Internal, not shown to users.
  readonly key: string;
  // Short human-friendly title (e.g. "Ftrace buffer too small").
  readonly title: string;
  readonly description: string;
  readonly remediation: string;
  readonly confidence: number;
  readonly traceId?: number;
  // File name of the trace this diagnostic belongs to (may be absent).
  readonly traceName?: string;
  // Size of the trace file in bytes (may be absent).
  readonly traceSize?: number;
}

const DIAGNOSTIC_SPEC = {
  key: STR,
  title: STR,
  description: STR,
  remediation: STR,
  confidence: NUM,
  traceId: NUM_NULL,
  traceName: STR_NULL,
  traceSize: LONG_NULL,
};

// Loads all rows of the `trace_diagnostics` table, most severe first. Uses
// tryQuery and returns an empty list if the table isn't available, so the UI
// degrades gracefully against older TraceProcessor versions.
export async function loadTraceDiagnostics(
  engine: Engine,
): Promise<ReadonlyArray<Diagnostic>> {
  const res = await engine.tryQuery(`
    select d.key, d.title, d.description, d.remediation, d.confidence,
           d.trace_id as traceId, f.name as traceName, f.size as traceSize
    from __intrinsic_trace_diagnostics d
    left join __intrinsic_trace_file f on f.id = d.trace_id
    order by d.confidence desc, d.key
  `);
  if (!res.ok) {
    return [];
  }
  const result = res.value;
  const diagnostics: Diagnostic[] = [];
  for (const it = result.iter(DIAGNOSTIC_SPEC); it.valid(); it.next()) {
    diagnostics.push({
      key: it.key,
      title: it.title,
      description: it.description,
      remediation: it.remediation,
      confidence: it.confidence,
      traceId: it.traceId ?? undefined,
      traceName: it.traceName ?? undefined,
      traceSize: it.traceSize !== null ? Number(it.traceSize) : undefined,
    });
  }
  return diagnostics;
}

export function hasHighSeverityDiagnostic(
  diagnostics: ReadonlyArray<Diagnostic>,
): boolean {
  return diagnostics.some((d) => d.confidence > HIGH_CONFIDENCE_THRESHOLD);
}

// Collapses diagnostics that share the same key (the same problem detected in
// several traces) down to one representative, keeping the first occurrence -
// i.e. the highest confidence, since the list is ordered by confidence desc.
// Used by the summarising surfaces (overview page + details drawer); the Trace
// Doctor tab intentionally keeps every per-trace entry with its full path.
export function dedupeDiagnosticsByKey(
  diagnostics: ReadonlyArray<Diagnostic>,
): ReadonlyArray<Diagnostic> {
  const seen = new Set<string>();
  const out: Diagnostic[] = [];
  for (const d of diagnostics) {
    if (seen.has(d.key)) continue;
    seen.add(d.key);
    out.push(d);
  }
  return out;
}

// Continuous severity colour ramp (yellow -> orange -> red).
function severityColor(confidence: number): string {
  const hue = 60 * (1 - clamp(confidence, 0, 1)); // 60=yellow .. 0=red.
  return `hsl(${hue}, 85%, 45%)`;
}

// Bucketed severity colour for the overview card border: yellow (<0.3),
// orange (0.3-0.7), red (>=0.7).
function severityBucketColor(confidence: number): string {
  if (confidence < 0.3) return 'hsl(48, 90%, 50%)'; // yellow
  if (confidence < 0.7) return 'hsl(28, 90%, 52%)'; // orange
  return 'hsl(0, 75%, 50%)'; // red
}

// A human-friendly label for the trace a diagnostic belongs to, used to
// disambiguate in the multi-trace case: the file name (or "Trace N" if the
// name is unknown), followed by the file size in MB when available.
function traceLabel(diagnostic: Diagnostic): string | undefined {
  if (diagnostic.traceId === undefined) return undefined;
  const name = diagnostic.traceName ?? `Trace ${diagnostic.traceId}`;
  if (diagnostic.traceSize === undefined) return name;
  const mb = diagnostic.traceSize / (1024 * 1024);
  return `${name} (${mb.toFixed(1)} MB)`;
}

function hasFix(diagnostic: Diagnostic): boolean {
  return diagnostic.remediation.trim().length > 0;
}

// A horizontal mini score bar whose width + colour come from the confidence.
function renderMiniBar(confidence: number): m.Children {
  const color = severityColor(confidence);
  const pct = Math.round(clamp(confidence, 0, 1) * 100);
  return m(
    '.pf-trace-doctor__mini-bar',
    {title: `Confidence: ${confidence.toFixed(2)}`},
    m('.pf-trace-doctor__mini-bar-fill', {
      style: {width: `${pct}%`, backgroundColor: color},
    }),
  );
}

// A small card header: a title on the left and the mini score bar on the right.
function renderCardHeader(title: string, confidence: number): m.Children {
  return m(
    '.pf-trace-doctor__issue-head',
    m('.pf-trace-doctor__issue-head-title', title),
    renderMiniBar(confidence),
  );
}

// ============================================================================
// Overview page: one clickable card per diagnostic. Header "Trace config
// issue" + mini score bar; body is the short title. Severity also shown by the
// coloured left border.
// ============================================================================

export interface DiagnosticCardAttrs {
  readonly diagnostic: Diagnostic;
  readonly onclick: () => void;
}

export class DiagnosticCard implements m.ClassComponent<DiagnosticCardAttrs> {
  view({attrs}: m.CVnode<DiagnosticCardAttrs>): m.Children {
    const {diagnostic} = attrs;
    return m(
      Card,
      {
        className:
          'pf-trace-info-page__diagnostic-card ' +
          'pf-trace-info-page__status-card--clickable',
        style: {borderLeftColor: severityBucketColor(diagnostic.confidence)},
        onclick: attrs.onclick,
      },
      renderCardHeader('Trace config issue', diagnostic.confidence),
      m(
        '.pf-trace-info-page__diagnostic-body',
        m('.pf-trace-info-page__diagnostic-description', diagnostic.title),
      ),
    );
  }
}

// ============================================================================
// In-page "Trace Doctor" sub-tab: one card per problem — header is the title +
// mini score bar; body is description on the left and the suggested fix on the
// right.
// ============================================================================

interface RenderProblemsOpts {
  readonly showTrace: boolean;
}

function renderProblem(d: Diagnostic, opts: RenderProblemsOpts): m.Children {
  const label = opts.showTrace ? traceLabel(d) : undefined;
  return m(
    '.pf-trace-info-page__diagnostic-row',
    // Left: the problem card (mini bar + title, then description).
    m(
      '.pf-trace-info-page__diagnostic-problem',
      m(
        '.pf-trace-info-page__diagnostic-problem-head',
        renderMiniBar(d.confidence),
        m('.pf-trace-doctor__issue-head-title', d.title),
      ),
      label !== undefined && m('.pf-trace-info-page__diagnostic-trace', label),
      m('.pf-trace-info-page__diagnostic-description', d.description),
    ),
    // Right: the suggested fix as a separate, equal-height card.
    hasFix(d) &&
      m(
        '.pf-trace-info-page__diagnostic-fix',
        m('.pf-trace-info-page__diagnostic-fix-head', 'Suggested fix'),
        m('.pf-trace-info-page__diagnostic-fix-body', d.remediation),
      ),
  );
}

function renderProblems(
  diagnostics: ReadonlyArray<Diagnostic>,
  opts: RenderProblemsOpts,
): m.Children {
  return diagnostics.map((d) => renderProblem(d, opts));
}

export interface TraceDoctorTabAttrs {
  readonly diagnostics: ReadonlyArray<Diagnostic>;
  readonly isMultiTrace: boolean;
}

export class TraceDoctorTab implements m.ClassComponent<TraceDoctorTabAttrs> {
  view({attrs}: m.CVnode<TraceDoctorTabAttrs>): m.Children {
    return m(
      '.pf-trace-info-page__tab-content',
      m(
        Section,
        {
          title: 'Trace Doctor',
          subtitle:
            'Problems detected in the trace config or environment that can ' +
            'affect the quality of the trace, with suggested fixes',
        },
        renderProblems(attrs.diagnostics, {
          showTrace: attrs.isMultiTrace,
        }),
      ),
    );
  }
}

// ============================================================================
// Details drawer: a compact union of the Trace Health status cards and the
// diagnostic cards, as tiles.
// ============================================================================

// A "Trace Health" status item (import errors, trace errors, data losses, ...).
export interface HealthItem {
  readonly title: string;
  readonly count: number;
  readonly severity: 'success' | 'danger' | 'warning' | 'notice';
  readonly icon: string;
  readonly onclick?: () => void;
}

function healthColor(item: HealthItem): string {
  // 'notice' has no dedicated colour var; mirror the overview status card,
  // which uses the primary accent for notices.
  if (item.severity === 'notice') return 'var(--pf-color-primary)';
  return `var(--pf-color-${item.severity})`;
}

interface UnionOpts {
  readonly health: ReadonlyArray<HealthItem>;
  readonly diagnostics: ReadonlyArray<Diagnostic>;
  // Called with a diagnostic's index when its tile is clicked.
  readonly onDiagnosticClick: (index: number) => void;
}

// Compact tiles in a grid, with a severity-coloured top border. Health items
// are single-width (icon + title + count); diagnostics are double-width with a
// "Trace config issue" header (+ mini score bar) and the short title as body.
// Every tile is clickable.
function renderUnionTiles(o: UnionOpts): m.Children {
  return m(
    '.pf-trace-info-doctor-details-tab__tiles',
    ...o.health.map((h) =>
      m(
        '.pf-trace-info-doctor-details-tab__tile' +
          '.pf-trace-info-doctor-details-tab__tile--clickable',
        {onclick: h.onclick, style: {borderTopColor: healthColor(h)}},
        m(Icon, {icon: h.icon, filled: true}),
        m('.pf-trace-info-doctor-details-tab__tile-title', h.title),
        m('.pf-trace-info-doctor-details-tab__tile-value', h.count),
      ),
    ),
    ...o.diagnostics.map((d, index) =>
      m(
        '.pf-trace-info-doctor-details-tab__tile' +
          '.pf-trace-info-doctor-details-tab__tile--wide' +
          '.pf-trace-info-doctor-details-tab__tile--clickable',
        {
          onclick: () => o.onDiagnosticClick(index),
          style: {borderTopColor: severityBucketColor(d.confidence)},
          title: d.remediation,
        },
        renderCardHeader('Trace config issue', d.confidence),
        m('.pf-trace-info-doctor-details-tab__tile-desc', d.title),
      ),
    ),
  );
}

export interface TraceDoctorDrawerAttrs {
  readonly diagnostics: ReadonlyArray<Diagnostic>;
  readonly health: ReadonlyArray<HealthItem>;
  readonly onDiagnosticClick: (index: number) => void;
}

export class TraceDoctorDrawer
  implements m.ClassComponent<TraceDoctorDrawerAttrs>
{
  view({attrs}: m.CVnode<TraceDoctorDrawerAttrs>): m.Children {
    return m(
      '.pf-trace-info-doctor-details-tab',
      m(
        '.pf-trace-info-doctor-details-tab__intro',
        'This trace is experiencing one or more trace data quality issues. ' +
          'Click on the cards to find out more.',
      ),
      renderUnionTiles({
        health: attrs.health,
        diagnostics: attrs.diagnostics,
        onDiagnosticClick: attrs.onDiagnosticClick,
      }),
    );
  }
}

// Loads diagnostics + non-zero trace-health metrics and, if there is anything
// worth showing, registers and auto-opens the "Trace Doctor" details-panel tab.
export async function maybeDisplayTraceDoctorTab(trace: Trace): Promise<void> {
  // Navigate to the Overview page's relevant sub-tab (hash routing).
  const goToTab = (tab: string) => trace.navigate(`#!/info/${tab}`);
  const diagnostics = await loadTraceDiagnostics(trace.engine);
  const overview = await loadOverviewData(trace);
  // Only surface health metrics that are actually non-zero (not green).
  const health: HealthItem[] = createStatusCards(overview)
    .filter(
      (c) =>
        c.count > 0 &&
        (c.title.toLowerCase().includes('errors') ||
          c.title.toLowerCase().includes('losses')),
    )
    .map((c) => ({
      title: c.title,
      count: c.count,
      severity: c.severity,
      icon: c.icon,
      onclick: () => goToTab(c.targetTab),
    }));

  // Auto-open the details panel whenever there is something worth showing: a
  // high-severity config problem or any non-zero trace-health metric (data
  // losses, errors, ...).
  if (!hasHighSeverityDiagnostic(diagnostics) && health.length === 0) {
    return;
  }
  const DIAGNOSTICS_TAB_URI = 'dev.perfetto.TraceInfoPage#TraceDiagnostics';

  trace.tabs.registerTab({
    uri: DIAGNOSTICS_TAB_URI,
    isEphemeral: false,
    content: {
      getTitle: () => 'Trace Doctor',
      render: () =>
        m(TraceDoctorDrawer, {
          // The drawer summarises, so collapse the same problem across traces.
          diagnostics: dedupeDiagnosticsByKey(diagnostics),
          health,
          onDiagnosticClick: () => goToTab('trace_doctor'),
        }),
    },
  });
  trace.tabs.showTab(DIAGNOSTICS_TAB_URI);
}
