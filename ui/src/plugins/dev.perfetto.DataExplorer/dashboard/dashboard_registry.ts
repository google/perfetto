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

import {PerfettoSqlType} from '../../../trace_processor/perfetto_sql_type';
import {SqlValue} from '../../../trace_processor/query_result';
import {ChartConfig} from '../query_builder/nodes/visualisation_node';

/**
 * A data source exported by a DashboardNode in the graph builder.
 * Global — not tied to any specific dashboard.
 */
export interface DashboardDataSource {
  readonly name: string;
  readonly nodeId: string;
  readonly columns: ReadonlyArray<{name: string; type?: PerfettoSqlType}>;
  // Stable ID of the graph tab that owns this source.
  readonly graphId: string;
  // Materialized SQL table name. Undefined until the source node has been
  // executed. DashboardNode resolves this eagerly on publish/update.
  tableName?: string;
  // Trigger execution of this source node (sync + materialize).
  requestExecution?: () => Promise<void>;
}

/** A data source enriched with its owning graph tab's display name. */
export type DashboardSourceWithName = DashboardDataSource & {
  graphName: string;
};

/**
 * Build a display label for a data source, optionally appending the graph name.
 *
 * When `forceNamespace` is true (sidebar), the graph name is shown whenever
 * the available sources span more than one graph.
 *
 * When `forceNamespace` is false (chart labels), the graph name is shown only
 * when the charts currently in use pull from more than one graph.
 */
export function sourceDisplayName(
  source: DashboardSourceWithName,
  items: ReadonlyArray<DashboardItem>,
  sources: ReadonlyArray<DashboardSourceWithName>,
  forceNamespace = false,
): string {
  let candidates: ReadonlyArray<DashboardSourceWithName>;
  if (forceNamespace) {
    candidates = sources;
  } else {
    const usedNodeIds = new Set(
      items.filter((i) => i.kind === 'chart').map((i) => i.sourceNodeId),
    );
    candidates = sources.filter((s) => usedNodeIds.has(s.nodeId));
  }
  const graphs = new Set(candidates.map((s) => s.graphId));
  if (graphs.size > 1) {
    return `${source.name} · ${source.graphName}`;
  }
  return source.name;
}

/** A single chart on a dashboard, linked to its data source. */
export interface DashboardChart {
  readonly sourceNodeId: string;
  config: ChartConfig;
  widthPx?: number;
  heightPx?: number;
  x?: number;
  y?: number;
}

/** A dashboard canvas item (currently only charts). */
export type DashboardItem = {readonly kind: 'chart'} & DashboardChart;

/** Get the unique ID for a dashboard item. */
export function getItemId(item: DashboardItem): string {
  return item.config.id;
}

/** A brush filter applied by interacting with a dashboard chart. */
export interface DashboardBrushFilter {
  readonly column: string;
  readonly op: '=' | '>=' | '<' | 'is null';
  readonly value?: SqlValue;
}

/**
 * Global pool of exported data sources.
 *
 * DashboardNode publishes sources here during query execution (outside the
 * Mithril render cycle), so this must be a global singleton. Dashboard tabs
 * read from this pool via DataExplorer, which passes sources as props.
 *
 * All other dashboard state (items, brush filters) lives on DataExplorerTab.
 */
class ExportedSourcesPool {
  private sources = new Map<string, DashboardDataSource>();

  /** Publish a data source from a DashboardNode. */
  setExportedSource(source: DashboardDataSource): void {
    this.sources.set(source.nodeId, source);
  }

  /** Remove an exported source (e.g. node deleted). */
  removeExportedSource(nodeId: string): void {
    this.sources.delete(nodeId);
  }

  /** Get a single exported source by node ID. */
  getExportedSource(nodeId: string): DashboardDataSource | undefined {
    return this.sources.get(nodeId);
  }

  /** Get all exported sources. */
  getAllExportedSources(): ReadonlyArray<DashboardDataSource> {
    return [...this.sources.values()];
  }

  clear(): void {
    this.sources.clear();
  }
}

export const dashboardRegistry = new ExportedSourcesPool();

const GRID_SIZE = 20;

export function snapToGrid(value: number): number {
  return Math.round(value / GRID_SIZE) * GRID_SIZE;
}

export function getNextItemPosition(items: ReadonlyArray<DashboardItem>): {
  x: number;
  y: number;
} {
  const offset = (items.length % 10) * 40;
  return {x: snapToGrid(20 + offset), y: snapToGrid(20 + offset)};
}

/**
 * Serialized dashboard data for persistence (localStorage / permalinks).
 */
export interface SerializedDashboardData {
  dashboardId?: string;
  dashboardItems?: unknown[];
  brushFilters?: Record<string, unknown[]>;
}

/**
 * Collect dashboard-related data for a tab into a serializable form.
 * Shared between localStorage persistence and permalink persistence.
 */
export function serializeDashboardData(
  dashboardId: string | undefined,
  items?: DashboardItem[],
  brushFiltersMap?: Map<string, DashboardBrushFilter[]>,
): SerializedDashboardData {
  if (dashboardId === undefined) return {};
  const dashboardItems =
    items !== undefined && items.length > 0 ? (items as unknown[]) : undefined;
  // Convert brush filters map to a plain record, bigint → number.
  let brushFilters: Record<string, unknown[]> | undefined;
  if (brushFiltersMap !== undefined && brushFiltersMap.size > 0) {
    const raw: Record<string, DashboardBrushFilter[]> = {};
    for (const [sourceNodeId, filters] of brushFiltersMap) {
      raw[sourceNodeId] = filters;
    }
    brushFilters = JSON.parse(
      JSON.stringify(raw, (_k, v) => (typeof v === 'bigint' ? Number(v) : v)),
    );
  }
  return {dashboardId, dashboardItems, brushFilters};
}

/**
 * Validate that an unknown value looks like a DashboardItem[].
 * Returns undefined if validation fails.
 */
export function validateDashboardItems(
  items: unknown[] | undefined,
): DashboardItem[] | undefined {
  if (items === undefined || items.length === 0) return undefined;
  const validated: DashboardItem[] = [];
  for (const item of items) {
    if (typeof item !== 'object' || item === null) continue;
    const obj = item as Record<string, unknown>;
    if (obj.kind === 'chart') {
      if (
        typeof obj.sourceNodeId !== 'string' ||
        typeof obj.config !== 'object' ||
        obj.config === null
      ) {
        continue;
      }
      const cfg = obj.config as Record<string, unknown>;
      if (
        typeof cfg.id !== 'string' ||
        typeof cfg.column !== 'string' ||
        typeof cfg.chartType !== 'string'
      ) {
        continue;
      }
      validated.push(item as DashboardItem);
    }
  }
  return validated.length > 0 ? validated : undefined;
}

/**
 * Parse serialized brush filters into a Map.
 * Validates each entry and drops invalid ones.
 */
export function parseBrushFilters(
  raw: Record<string, unknown[]>,
): Map<string, DashboardBrushFilter[]> {
  const validOps = new Set(['=', '>=', '<', 'is null']);
  const result = new Map<string, DashboardBrushFilter[]>();
  for (const [sourceNodeId, entries] of Object.entries(raw)) {
    const validated: DashboardBrushFilter[] = [];
    for (const entry of entries) {
      if (typeof entry !== 'object' || entry === null) continue;
      const obj = entry as Record<string, unknown>;
      if (typeof obj.column !== 'string') continue;
      if (typeof obj.op !== 'string' || !validOps.has(obj.op)) continue;
      validated.push(entry as DashboardBrushFilter);
    }
    if (validated.length > 0) {
      result.set(sourceNodeId, validated);
    }
  }
  return result;
}
