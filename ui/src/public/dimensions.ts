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

// Shared presentation layer for track *dimensions*.
//
// A dimension is a named, typed key/value which is part of a track's identity
// (see the `track_dimension` table in trace processor). Machine, GPU, CPU,
// process, thread and producer-declared dimensions such as `rank` are all
// instances of the same concept, so they should also be *labelled* by one piece
// of code rather than one hand-rolled implementation each.
//
// The generic collapse pass which turns these labels into track subtitles lives
// in the dev.perfetto.TrackDimensions plugin.

// A single effective dimension of a track/process/thread, as returned by trace
// processor.
export interface Dimension {
  readonly name: string;
  readonly intValue?: number | null;
  readonly stringValue?: string | null;
  readonly displayName?: string | null;
}

// Dimensions which already have a dedicated presentation elsewhere in the UI:
// GPU gets its own hierarchy level and machine is a track name suffix, so the
// shared collapse pass must not label them a second time.
//
// TODO(dreveman): once those two consume this module, machine moves into the
// subtitle and only `gpu` stays here, as described in the RFC.
const PRESENTED_ELSEWHERE = new Set([
  'machine',
  'gpu',
  'cpu',
  'process',
  'thread',
]);

// The canonical value of a dimension, as a string. This is what the collapse
// pass counts distinct values of, and what queries join on.
export function dimensionValue(dimension: Dimension): string {
  if (dimension.intValue !== undefined && dimension.intValue !== null) {
    return String(dimension.intValue);
  }
  return dimension.stringValue ?? '';
}

/**
 * Format a dimension as a label, e.g. `rank 3` or `worker-east`.
 *
 * A producer-supplied display name overrides the rendered label but never the
 * canonical value used for queries or merging.
 */
export function formatDimensionLabel(dimension: Dimension): string {
  const displayName = dimension.displayName;
  if (displayName !== undefined && displayName !== null && displayName !== '') {
    return displayName;
  }
  return `${dimension.name} ${dimensionValue(dimension)}`;
}

/**
 * Format a set of dimensions as a single subtitle string, e.g.
 * `rank 3 · stage forward`. Dimensions are expected to be pre-filtered by the
 * collapse rule.
 */
export function formatDimensionLabels(
  dimensions: ReadonlyArray<Dimension>,
): string {
  return dimensions.map(formatDimensionLabel).join(' · ');
}

/**
 * The shared collapse rule, which decides which dimensions are labelled:
 *  - integer dimensions are identifiers, so they are only worth showing when
 *    they disambiguate something, i.e. when the trace has more than one
 *    distinct value for them. E.g. the `rank` of a trace recorded by a single
 *    training process is not labelled.
 *  - string dimensions are always labelled: they are typically emitted to be
 *    shown, e.g. the frame titles of a Chrome renderer process.
 * Dimensions which already have a specialized presentation are never labelled
 * here.
 *
 * @param dimensions Every effective dimension in the trace.
 * @returns The names of the dimensions which should be labelled.
 */
export function visibleDimensionNames(
  dimensions: ReadonlyArray<Dimension>,
): Set<string> {
  const values = new Map<string, Set<string>>();
  const visible = new Set<string>();
  for (const dimension of dimensions) {
    if (PRESENTED_ELSEWHERE.has(dimension.name)) continue;
    if (dimension.intValue === undefined || dimension.intValue === null) {
      visible.add(dimension.name);
      continue;
    }
    const distinct = values.get(dimension.name) ?? new Set<string>();
    distinct.add(dimensionValue(dimension));
    values.set(dimension.name, distinct);
  }
  for (const [name, distinct] of values) {
    if (distinct.size > 1) visible.add(name);
  }
  return visible;
}
