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

/** A single chart on a dashboard, linked to its data source. */
export interface DashboardChart {
  readonly sourceNodeId: string;
  config: ChartConfig;
  widthPx?: number;
  heightPx?: number;
  x?: number;
  y?: number;
}

/** An editable text label on the dashboard canvas. */
export interface DashboardLabel {
  readonly id: string;
  readonly text: string;
  widthPx?: number;
  heightPx?: number;
  x?: number;
  y?: number;
}

/** A dashboard canvas item — either a chart or a label. */
export type DashboardItem =
  | ({readonly kind: 'chart'} & DashboardChart)
  | ({readonly kind: 'label'} & DashboardLabel);

/** Get the unique ID for a dashboard item. */
export function getItemId(item: DashboardItem): string {
  return item.kind === 'chart' ? item.config.id : item.id;
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

  /** Get exported sources belonging to a specific graph tab. */
  getExportedSourcesForGraph(
    graphId: string,
  ): ReadonlyArray<DashboardDataSource> {
    return [...this.sources.values()].filter((s) => s.graphId === graphId);
  }

  clear(): void {
    this.sources.clear();
  }
}

export const dashboardRegistry = new ExportedSourcesPool();

/**
 * Returns the nodeIds of all sources that have a column named `column`,
 * including `sourceNodeId` itself.
 */
export function getLinkedSourceNodeIds(
  sources: ReadonlyArray<Pick<DashboardDataSource, 'nodeId' | 'columns'>>,
  sourceNodeId: string,
  column: string,
): string[] {
  return sources
    .filter(
      (s) =>
        s.nodeId === sourceNodeId || s.columns.some((c) => c.name === column),
    )
    .map((s) => s.nodeId);
}

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
 * Convert dashboard items to a serializable array, or undefined if empty.
 */
export function serializeDashboardItems(
  items?: DashboardItem[],
): unknown[] | undefined {
  return items !== undefined && items.length > 0
    ? (items as unknown[])
    : undefined;
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
    } else if (obj.kind === 'label') {
      if (typeof obj.id !== 'string' || typeof obj.text !== 'string') continue;
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
