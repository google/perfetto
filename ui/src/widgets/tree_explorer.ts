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

// Shared types, state and filter grammar for the tree explorer: the family of
// widgets (filter bar, flamegraph canvas and, in future, tree/flat table
// views) that together visualize weighted tree data such as callstacks.

import type m from 'mithril';
import {z} from 'zod';
import {assertUnreachable} from '../base/assert';
import {parseUserFilterRegex} from './flamegraph_regex';

// Context passed to a TreeExplorerOptionalAction's execute callback.
//
// `properties` is the (reduced) kv map of the user-declared
// unaggregatableProperties / aggregatableProperties on the metric.
//
// `node` is the clicked node for node-level actions, and undefined for
// root-level actions (where there is no specific node).
export interface TreeExplorerActionContext {
  readonly properties: ReadonlyMap<string, string>;
  readonly node?: TreeExplorerNode;
}

export interface TreeExplorerOptionalAction {
  readonly name: string;
  execute?: (ctx: TreeExplorerActionContext) => void;
  readonly subActions?: TreeExplorerOptionalAction[];
  // Presentation in the categorized node menu; absent category → "Drill down".
  readonly icon?: string;
  readonly description?: m.Children;
  readonly category?: ActionCategory;
}

// FOCUS re-frames without removing data; FILTER reshapes what's shown; DRILL
// inspects elsewhere; COPY exports.
export type ActionCategory = 'FOCUS' | 'FILTER' | 'DRILL' | 'COPY';

export interface TreeExplorerOptionalMarker {
  readonly name: string;
  isVisible: (properties: ReadonlyMap<string, string>) => boolean;
}

export type TreeExplorerPropertyDefinition = {
  displayName: string;
  value: string;
  isVisible: boolean;
  isAggregatable: boolean;
};

export interface TreeExplorerNode {
  readonly id: number;
  readonly parentId: number;
  readonly depth: number;
  readonly name: string;
  readonly selfValue: number;
  readonly cumulativeValue: number;
  readonly parentCumulativeValue?: number;
  readonly properties: ReadonlyMap<string, TreeExplorerPropertyDefinition>;
  readonly marker?: string;
  readonly xStart: number;
  readonly xEnd: number;
}

export interface TreeExplorerData {
  readonly nodes: ReadonlyArray<TreeExplorerNode>;
  readonly unfilteredCumulativeValue: number;
  readonly allRootsCumulativeValue: number;
  readonly minDepth: number;
  readonly maxDepth: number;
  readonly nodeActions: ReadonlyArray<TreeExplorerOptionalAction>;
  readonly rootActions: ReadonlyArray<TreeExplorerOptionalAction>;
}

const TREE_EXPLORER_FILTER_SCHEMA = z
  .object({
    kind: z
      .union([
        z.literal('SHOW_STACK').readonly(),
        z.literal('HIDE_STACK').readonly(),
        z.literal('HIDE_FRAME').readonly(),
        z.literal('OPTIONS').readonly(),
      ])
      .readonly(),
    filter: z.string().readonly(),
  })
  .readonly();

export type TreeExplorerFilter = z.infer<typeof TREE_EXPLORER_FILTER_SCHEMA>;

const TREE_EXPLORER_VIEW_SCHEMA = z
  .discriminatedUnion('kind', [
    z.object({kind: z.literal('TOP_DOWN').readonly()}),
    z.object({kind: z.literal('BOTTOM_UP').readonly()}),
    z.object({
      kind: z.literal('FROM_FRAME').readonly(),
      pattern: z.string().readonly(),
      displayLabel: z.string().optional().readonly(),
    }),
    z.object({
      kind: z.literal('PIVOT').readonly(),
      pivot: z.string().readonly(),
      // Display text for the pivot chip; SQL match still uses `pivot`.
      displayLabel: z.string().optional().readonly(),
    }),
  ])
  .readonly();

export type TreeExplorerView = z.infer<typeof TREE_EXPLORER_VIEW_SCHEMA>;

export const TREE_EXPLORER_STATE_SCHEMA = z
  .object({
    selectedMetricId: z.string().readonly(),
    addedMetricIds: z.array(z.string()).default([]),
    // How the tree is displayed: the flamegraph canvas, an expandable call
    // tree table or a flat per-function table. Defaulted so state persisted
    // before this field existed keeps parsing.
    displayMode: z.enum(['flamegraph', 'tree', 'flat']).default('flamegraph'),
    filters: z.array(TREE_EXPLORER_FILTER_SCHEMA),
    view: TREE_EXPLORER_VIEW_SCHEMA,
  })
  .readonly();

export type TreeExplorerState = z.infer<typeof TREE_EXPLORER_STATE_SCHEMA>;
export type TreeExplorerDisplayMode = TreeExplorerState['displayMode'];

export interface TreeExplorerMetric {
  // Stable identity used in persisted state. Defaults to `name`.
  readonly id?: string;
  readonly name: string;
  readonly unit: string;
  // Where the measure came from. Undefined is treated as ADDED so callers
  // only need to mark the small set of built-in defaults.
  readonly provenance?: 'DEFAULT' | 'ADDED';
  // Label for the name column in copy stack table and tooltip.
  // Examples: "Symbol", "Slice", "Class". Defaults to "Name".
  readonly nameColumnLabel?: string;
}

export interface TreeExplorerAddableMetric {
  readonly id: string;
  readonly name: string;
}

export function metricId(metric: TreeExplorerMetric): string {
  return metric.id ?? metric.name;
}

export type FilterType =
  'SHOW_STACK' | 'HIDE_STACK' | 'SHOW_FROM_FRAME' | 'HIDE_FRAME' | 'PIVOT';
export type PatternViewKind = 'FROM_FRAME' | 'PIVOT';

export interface FilterTypeOption {
  readonly value: FilterType;
  // Canonical name; also a valid filter-bar syntax prefix.
  readonly label: string;
  readonly friendlyLabel: string;
  readonly shortLabel: string;
  readonly icon: string;
  readonly category: ActionCategory;
  readonly description: string;
  // Example pattern used in tips for this filter type.
  readonly example: string;
  // Name used by other profilers, if any; surfaced in the node menu.
  readonly aka?: string;
}

export const FILTER_TYPES: ReadonlyArray<FilterTypeOption> = [
  {
    value: 'SHOW_STACK',
    label: 'Show Stack',
    friendlyLabel: 'Keep stacks matching name',
    shortLabel: 'SS',
    example: 'HandleRequest',
    icon: 'visibility',
    category: 'FILTER',
    description:
      'Keep only samples whose stack contains a frame whose name matches.',
  },
  {
    value: 'HIDE_STACK',
    label: 'Hide Stack',
    friendlyLabel: 'Hide stacks matching name',
    shortLabel: 'HS',
    example: 'malloc',
    icon: 'visibility_off',
    category: 'FILTER',
    description:
      'Remove samples whose stack contains a frame whose name matches.',
    aka: 'Drop function',
  },
  {
    value: 'SHOW_FROM_FRAME',
    label: 'Show From Frame',
    friendlyLabel: 'Show from matching frame',
    shortLabel: 'SFF',
    example: 'HandleRequest',
    icon: 'center_focus_strong',
    category: 'FOCUS',
    description:
      'Re-root at matching frames and show their descendants, dropping ancestors.',
    aka: 'Focus on subtree',
  },
  {
    value: 'HIDE_FRAME',
    label: 'Hide Frame',
    friendlyLabel: 'Merge matching frames into caller',
    shortLabel: 'HF',
    example: '/.*alloc.*/i',
    icon: 'call_merge',
    category: 'FILTER',
    description:
      'Remove frames whose name matches, merging their children into the caller.',
    aka: 'Merge function',
  },
  {
    value: 'PIVOT',
    label: 'Pivot',
    friendlyLabel: 'Pivot on matching frames',
    shortLabel: 'P',
    example: '/.*alloc.*/i',
    icon: 'account_tree',
    category: 'FOCUS',
    description:
      'Re-root at matching frames with callers above and callees below.',
  },
];

export function createDefaultTreeExplorerState(
  metrics: ReadonlyArray<TreeExplorerMetric>,
): TreeExplorerState {
  return {
    selectedMetricId: metricId(metrics[0]),
    addedMetricIds: [],
    displayMode: 'flamegraph',
    filters: [],
    view: {kind: 'TOP_DOWN'},
  };
}

/**
 * Updates a TreeExplorerState with new metrics, preserving filters where
 * possible.
 *
 * If the current state has no metric selected (empty string), this will
 * initialize it with the first metric. Otherwise, it preserves the selected
 * metric if it still exists in the new metrics array, or falls back to the
 * first metric if it doesn't.
 *
 * Returns `state` unchanged (same reference) when it is already valid:
 * callers rely on reference stability to avoid spurious data refetches.
 */
export function updateTreeExplorerState(
  state: TreeExplorerState | undefined,
  metrics: ReadonlyArray<TreeExplorerMetric>,
): TreeExplorerState {
  if (state === undefined) {
    return createDefaultTreeExplorerState(metrics);
  }
  const metricStillExists = metrics.some(
    (m) => metricId(m) === state.selectedMetricId,
  );
  if (metricStillExists) {
    return state;
  }
  return {
    filters: state.filters,
    view: state.view,
    addedMetricIds: state.addedMetricIds,
    displayMode: state.displayMode,
    selectedMetricId: metricId(metrics[0]),
  };
}

export function addFilter(
  state: TreeExplorerState,
  filter: TreeExplorerFilter,
): TreeExplorerState {
  return {
    ...state,
    filters: state.filters.concat([filter]),
  };
}

export function toTags(state: TreeExplorerState): ReadonlyArray<string> {
  const toString = (x: TreeExplorerFilter) => {
    switch (x.kind) {
      case 'HIDE_FRAME':
        return 'Hide Frame: ' + x.filter;
      case 'HIDE_STACK':
        return 'Hide Stack: ' + x.filter;
      case 'SHOW_STACK':
        return 'Show Stack: ' + x.filter;
      case 'OPTIONS':
        return 'Options';
    }
  };
  const filters = state.filters.map((x) => toString(x));
  switch (state.view.kind) {
    case 'FROM_FRAME':
      return filters.concat([
        'Show From Frame: ' + (state.view.displayLabel ?? state.view.pattern),
      ]);
    case 'PIVOT':
      return filters.concat([
        'Pivot: ' + (state.view.displayLabel ?? state.view.pivot),
      ]);
    case 'TOP_DOWN':
    case 'BOTTOM_UP':
      return filters;
    default:
      assertUnreachable(state.view);
  }
}

// Split text into individual filters by finding filter type prefixes
// e.g. 'Show Stack: main Hide Frame: alloc' -> ['Show Stack: main', 'Hide Frame: alloc']
// e.g. 'SS: foo HF: bar' -> ['SS: foo', 'HF: bar']
export function splitFilters(text: string): string[] {
  const lowerText = text.toLowerCase();

  // Find all positions where a filter prefix starts (case insensitive)
  const splitPositions: number[] = [];
  for (const type of FILTER_TYPES) {
    for (const prefix of [type.shortLabel, type.label]) {
      const searchStr = prefix.toLowerCase() + ':';
      let pos = 0;
      while ((pos = lowerText.indexOf(searchStr, pos)) !== -1) {
        // Only split if at start or preceded by whitespace
        if (pos === 0 || /\s/.test(text[pos - 1])) {
          splitPositions.push(pos);
        }
        pos += searchStr.length;
      }
    }
  }

  // Sort and deduplicate positions
  splitPositions.sort((a, b) => a - b);

  // If no prefixes found, return the whole text as one filter
  if (splitPositions.length === 0) {
    return text.trim() ? [text.trim()] : [];
  }

  // Split text at those positions
  const result: string[] = [];
  for (let i = 0; i < splitPositions.length; i++) {
    const start = splitPositions[i];
    const end = splitPositions[i + 1] ?? text.length;
    const part = text.substring(start, end).trim();
    if (part) {
      result.push(part);
    }
  }
  return result;
}

// Parse a filter string into type and value
// e.g. 'SS: main' -> {type: 'SHOW_STACK', value: 'main'}
// e.g. 'Show Stack: main' -> {type: 'SHOW_STACK', value: 'main'}
export function parseFilter(
  text: string,
  defaultType: FilterType = 'SHOW_STACK',
): {type: FilterType; value: string} {
  const i = text.indexOf(':');
  if (i === -1) return {type: defaultType, value: text};
  const prefix = text.substring(0, i).trim().toLowerCase();
  const value = text.substring(i + 1).trim();
  const match = FILTER_TYPES.find(
    (o) =>
      o.shortLabel.toLowerCase() === prefix || o.label.toLowerCase() === prefix,
  );
  return match ? {type: match.value, value} : {type: defaultType, value: text};
}

// Turns the user-facing highlight pattern into a RegExp, or undefined when the
// pattern is empty or not (yet) a valid regex.
export function computeHighlightRegex(pattern: string): RegExp | undefined {
  if (pattern === '') {
    return undefined;
  }
  try {
    const regex = parseUserFilterRegex(pattern);
    return new RegExp(regex.pattern, regex.flags);
  } catch {
    return undefined;
  }
}

// Formats a value in the metric's unit ('B' and 'ns' are human-scaled with
// 1024/1000 steps, 'count' and '' verbatim, anything else K/M/G-prefixed).
export function displaySize(totalSize: number, unit: string): string {
  if (unit === '' || unit === 'count') return totalSize.toLocaleString();
  if (totalSize === 0) return `0 ${unit}`;
  let step: number;
  let units: string[];
  switch (unit) {
    case 'B':
      step = 1024;
      units = ['B', 'KiB', 'MiB', 'GiB'];
      break;
    case 'ns':
      step = 1000;
      units = ['ns', 'us', 'ms', 's'];
      break;
    default:
      step = 1000;
      units = [unit, `K${unit}`, `M${unit}`, `G${unit}`];
      break;
  }
  const unitsIndex = Math.min(
    Math.trunc(Math.log(totalSize) / Math.log(step)),
    units.length - 1,
  );
  const pow = Math.pow(step, unitsIndex);
  const result = totalSize / pow;
  const resultString =
    totalSize % pow === 0 ? result.toString() : result.toFixed(2);
  return `${resultString} ${units[unitsIndex]}`;
}

export function displayPercentage(size: number, totalSize: number): string {
  if (totalSize === 0) {
    return `[NULL]%`;
  }
  return `${((size / totalSize) * 100.0).toFixed(2)}%`;
}

export function getUnitDisplayName(unit: string | undefined): string {
  if (unit === undefined || unit === '' || unit === 'count') {
    return 'count';
  }
  return unit;
}
