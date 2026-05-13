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

// The canvas is divided into a 24-column grid of square cells.
// Items snap to this grid for positioning and sizing.
export const GRID_COLUMNS = 24;
export const DEFAULT_COL_SPAN = 8;
export const DEFAULT_ROW_SPAN = 6;
export const MIN_COL_SPAN = 4;
export const MIN_ROW_SPAN = 4;
// Row span for divider items (must match inline style in dashboard.ts).
export const DIVIDER_ROW_SPAN = 0;
// Visual margin (in grid cells) from the canvas edges.
export const GRID_MARGIN = 0.5;

/** A single chart on a dashboard, linked to its data source. */
export interface DashboardChart {
  readonly sourceNodeId: string;
  config: ChartConfig;
  col?: number;
  row?: number;
  colSpan?: number;
  rowSpan?: number;
}

/** An editable text label on the dashboard canvas. */
export interface DashboardLabel {
  readonly id: string;
  readonly text: string;
  col?: number;
  row?: number;
  colSpan?: number;
  rowSpan?: number;
}

/**
 * A horizontal divider that splits the dashboard into segments.
 * Charts above a divider are "drivers" — they show brush selections visually
 * but do NOT filter their own data. Charts below a divider are "consumers" —
 * they get filtered by brush selections from driver charts above.
 */
export interface DashboardDivider {
  readonly id: string;
  /** Grid row of the divider (spans all 24 columns). */
  row: number;
  /** Optional label displayed on the divider. */
  label?: string;
}

/** A dashboard canvas item — a chart, label, or segment divider. */
export type DashboardItem =
  | ({readonly kind: 'chart'} & DashboardChart)
  | ({readonly kind: 'label'} & DashboardLabel)
  | ({readonly kind: 'divider'} & DashboardDivider);

/** Get the unique ID for a dashboard item. */
export function getItemId(item: DashboardItem): string {
  if (item.kind === 'chart') return item.config.id;
  return item.id;
}

/**
 * Return all chart items that drive `target` — i.e. charts whose brush
 * selections filter `target`'s SQL query.
 *
 * Chart X drives chart Y when there exists a divider D with X.row < D.row
 * and D.row <= Y.row.
 */
export function getDriversOf(
  target: DashboardItem,
  allItems: ReadonlyArray<DashboardItem>,
): DashboardItem[] {
  if (target.kind !== 'chart') return [];
  const targetRow = target.row ?? 0;
  const dividerRows: number[] = [];
  for (const i of allItems) {
    if (i.kind === 'divider' && i.row <= targetRow) {
      dividerRows.push(i.row);
    }
  }
  if (dividerRows.length === 0) return [];
  return allItems.filter((candidate) => {
    if (candidate.kind !== 'chart') return false;
    const cRow = candidate.row ?? 0;
    return dividerRows.some((dr) => cRow < dr);
  });
}

/**
 * Return all chart items that `source` drives — i.e. charts whose SQL
 * queries are filtered by `source`'s brush selections.
 */
export function getConsumersOf(
  source: DashboardItem,
  allItems: ReadonlyArray<DashboardItem>,
): DashboardItem[] {
  if (source.kind !== 'chart') return [];
  const sourceRow = source.row ?? 0;
  const dividerRows: number[] = [];
  for (const i of allItems) {
    if (i.kind === 'divider' && i.row > sourceRow) {
      dividerRows.push(i.row);
    }
  }
  if (dividerRows.length === 0) return [];
  return allItems.filter((candidate) => {
    if (candidate.kind !== 'chart') return false;
    const cRow = candidate.row ?? 0;
    return dividerRows.some((dr) => dr <= cRow);
  });
}

/**
 * A chart is a "driver" if it has consumers — i.e. there is a divider below
 * it. Driver charts show brush selection overlays but do NOT apply brush
 * filters to their own SQL queries.
 */
export function isDriverChart(
  item: DashboardItem,
  allItems: ReadonlyArray<DashboardItem>,
): boolean {
  return getConsumersOf(item, allItems).length > 0;
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

/** Get the bounding box of a dashboard item in grid coordinates. */
export function getItemBounds(item: DashboardItem): {
  col: number;
  row: number;
  colSpan: number;
  rowSpan: number;
} {
  if (item.kind === 'divider') {
    return {
      col: 0,
      row: item.row,
      colSpan: GRID_COLUMNS,
      rowSpan: DIVIDER_ROW_SPAN,
    };
  }
  return {
    col: item.col ?? 0,
    row: item.row ?? 0,
    colSpan: item.colSpan ?? DEFAULT_COL_SPAN,
    rowSpan: item.rowSpan ?? DEFAULT_ROW_SPAN,
  };
}

/**
 * Check if a rectangle overlaps any item except `skipId` (grid coords),
 * or would be directly adjacent (enforcing a 1-cell gap between items).
 */
export function checkOverlap(
  col: number,
  row: number,
  colSpan: number,
  rowSpan: number,
  items: ReadonlyArray<DashboardItem>,
  skipId: string,
): boolean {
  for (const item of items) {
    if (getItemId(item) === skipId) continue;
    const b = getItemBounds(item);
    // Items must have at least 1 grid cell between them.
    const overlaps = !(
      col + colSpan < b.col ||
      col > b.col + b.colSpan ||
      row + rowSpan < b.row ||
      row > b.row + b.rowSpan
    );
    if (overlaps) return true;
  }
  return false;
}

/**
 * Find the nearest non-overlapping position by scanning grid cells
 * row by row, left to right.
 */
export function findNonOverlappingPosition(
  startCol: number,
  startRow: number,
  colSpan: number,
  rowSpan: number,
  items: ReadonlyArray<DashboardItem>,
  skipId: string,
): {col: number; row: number} {
  // Clamp to grid bounds.
  startCol = Math.max(0, Math.min(GRID_COLUMNS - colSpan, startCol));
  startRow = Math.max(0, startRow);

  if (!checkOverlap(startCol, startRow, colSpan, rowSpan, items, skipId)) {
    return {col: startCol, row: startRow};
  }

  // Scan row by row, left to right, starting near the requested position.
  // The grid has a finite number of columns so each row is bounded; we scan
  // downward until a free slot is found (dashboards have a small number of
  // items so this terminates quickly).
  for (let row = startRow; ; row++) {
    for (let col = 0; col <= GRID_COLUMNS - colSpan; col++) {
      if (!checkOverlap(col, row, colSpan, rowSpan, items, skipId)) {
        return {col, row};
      }
    }
  }
}

/** Return a grid position for the next item, cascading left-to-right. */
export function getNextItemPosition(items: ReadonlyArray<DashboardItem>): {
  col: number;
  row: number;
} {
  const itemsPerRow = Math.floor(GRID_COLUMNS / DEFAULT_COL_SPAN);
  const idx = items.length;
  const col = (idx % itemsPerRow) * DEFAULT_COL_SPAN;
  const row = Math.floor(idx / itemsPerRow) * DEFAULT_ROW_SPAN;
  return {col, row};
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
    } else if (obj.kind === 'divider') {
      if (typeof obj.id !== 'string' || typeof obj.row !== 'number') continue;
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
