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

// Section interface and registry for GPU compute metric sections.
//
// Each Section declares the metrics it needs and how to render them.
// Sections are data-driven: metric rows are declared as static arrays
// with terminology placeholder functions for labels/units.
//
// Plugins can register new sections to extend the details view.

import type {Terminology} from '../terminology';

// =============================================================================
// Types
// =============================================================================

// A single metric row declaration.
export interface MetricRowDecl {
  // Canonical metric ID (used for lookup and as EXTRACT_ARG / counter key).
  readonly id: string;
  // Human-readable label; receives terminology so it can adapt.
  readonly label: (t: Terminology) => string;
  // Unit string; receives terminology so per-group denominator can adapt.
  readonly unit: (t: Terminology) => string;
  // Row importance level.
  // - 'required': always shown, used for visibility checks
  // - 'optional': shown when available but not critical
  readonly importance?: 'required' | 'optional';
  // How to aggregate the counter value for this metric.
  // Only meaningful for counter metrics (not launch args).
  // - 'sum': use the sum of counter values
  // - 'avg': use the average of counter values
  readonly aggregation?: 'sum' | 'avg';
}

// A table within a section.
export interface TableDecl {
  // Static description shown above the table. Receives terminology.
  readonly description: (t: Terminology) => string;
  // Metric rows in display order.
  readonly rows: readonly MetricRowDecl[];
}

// Returns true if a table should be visible given the available metrics.
// A table is visible when all 'required' rows have data available.
// Tables with no 'required' rows are always visible.
export function isTableVisible(
  table: TableDecl,
  availableMetrics: ReadonlySet<string>,
): boolean {
  return table.rows.every(
    (row) => row.importance !== 'required' || availableMetrics.has(row.id),
  );
}

// Interface that metric section providers must implement.
//
// A section represents a collapsible group of metric tables in the
// kernel details view (e.g. "Speed Of Light Throughput", "Occupancy").
export interface Section {
  // Unique identifier for the section.
  readonly id: string;

  // Display title shown in the collapsible section header.
  readonly title: string;

  // Launch metrics this section needs (extracted from slice args via
  // EXTRACT_ARG).
  // Example: ['launch__block_size', 'launch__grid_size', 'arch']
  readonly launchMetrics: readonly string[];

  // Counter metrics this section needs (from gpu_counter_track).
  // Example: ['sm__cycles_active.avg', 'gpu__time_duration.sum']
  readonly counterMetrics: readonly string[];

  // Whether this section should be collapsed by default.
  readonly collapsedByDefault?: boolean;

  // Optional display order for this section.
  // Lower values appear first. Sections without an order value are
  // placed after all ordered sections, in registration order.
  readonly order?: number;

  // Optional analysis prompt providing context for this section.
  // Explains what the metrics mean and how to interpret them.
  // The analysis provider may use this as a prompt for a language
  // model or ignore it entirely.
  readonly analysisPrompt?: string;

  // One or more tables that form this section's content.
  readonly tables: readonly TableDecl[];
}

// =============================================================================
// Section Registry
// =============================================================================

const sections: Section[] = [];

// Register a new metric section.
// Duplicate registrations (same id) are ignored.
export function registerSection(section: Section): void {
  if (!sections.some((s) => s.id === section.id)) {
    sections.push(section);
  }
}

// Get all registered sections sorted by {@link Section.order}.
// Sections with a lower order value appear first. Sections without an
// explicit order are placed after all ordered sections, preserving
// their original registration order.
export function getSections(): readonly Section[] {
  return [...sections].sort((a, b) => {
    if (a.order !== undefined && b.order !== undefined) {
      return a.order - b.order;
    }
    if (a.order !== undefined) return -1;
    if (b.order !== undefined) return 1;
    return 0;
  });
}

// Collect all unique launch metric IDs required by registered sections.
export function getAllLaunchMetrics(): string[] {
  const set = new Set<string>();
  for (const s of sections) {
    for (const metric of s.launchMetrics) set.add(metric);
  }
  return Array.from(set);
}

// Collect all unique counter metric IDs required by registered sections.
export function getAllCounterMetrics(): string[] {
  const set = new Set<string>();
  for (const s of sections) {
    for (const metric of s.counterMetrics) set.add(metric);
  }
  return Array.from(set);
}

// Build a map from counter metric ID to its declared aggregation type.
// Metrics without an explicit aggregation are not included.
export function getCounterAggregations(): ReadonlyMap<string, 'sum' | 'avg'> {
  const map = new Map<string, 'sum' | 'avg'>();
  for (const s of sections) {
    for (const table of s.tables) {
      for (const row of table.rows) {
        if (row.aggregation) {
          map.set(row.id, row.aggregation);
        }
      }
    }
  }
  return map;
}

// Look up the system prompt for a section by its display title.
export function getSectionSystemPrompt(title: string): string | undefined {
  return sections.find((s) => s.title === title)?.analysisPrompt;
}

// =============================================================================
// Well-known metric registry
// =============================================================================

// Registry mapping metric role names to candidate metric IDs.
// Plugins call registerWellKnownMetric() to add vendor-specific IDs
// for roles like 'duration', 'cycles', 'frequency', etc.
// The core plugin resolves the first available ID at render time.
const wellKnownMetrics = new Map<string, string[]>();

// Register one or more metric IDs for a well-known role.
// Later registrations append to existing ones; the first available
// ID at render time wins.
export function registerWellKnownMetric(
  role: string,
  ids: string | readonly string[],
): void {
  const list = wellKnownMetrics.get(role) ?? [];
  const toAdd = typeof ids === 'string' ? [ids] : ids;
  for (const id of toAdd) {
    if (!list.includes(id)) list.push(id);
  }
  wellKnownMetrics.set(role, list);
}

// Returns the first available metric ID for a role.
export function getWellKnownMetricId(
  role: string,
  availableMetrics: ReadonlySet<string>,
): string | undefined {
  const ids = wellKnownMetrics.get(role);
  if (ids === undefined) return undefined;
  return ids.find((id) => availableMetrics.has(id));
}

// Returns all registered metric IDs for a role
// (used by summary SQL to match any vendor).
export function getWellKnownMetricIds(role: string): string[] {
  return wellKnownMetrics.get(role) ?? [];
}
