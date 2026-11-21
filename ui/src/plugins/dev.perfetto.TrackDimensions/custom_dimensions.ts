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

import {getOrCreate} from '../../base/utils';
import type {Engine} from '../../trace_processor/engine';
import {
  LONG_NULL,
  NUM,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';

// One custom dimension of a trace processor track.
export interface CustomDimension {
  readonly name: string;
  // Identity of the typed value, so int64 and string values never collide.
  readonly key: string;
  // Chip text: the display name if present, else "<name> <value>".
  readonly label: string;
}

export interface CustomDimensions {
  // Custom dimensions by trace processor track id, then by dimension name.
  readonly byTrack: ReadonlyMap<number, ReadonlyMap<string, CustomDimension>>;
  // Dimension names with more than one distinct value in the trace. Values
  // uniform across the whole trace don't tell tracks apart, so we hide them.
  readonly varyingNames: ReadonlySet<string>;
  // Root track event tracks of each process, by upid. Their dimensions
  // describe the process as a whole.
  readonly processRootTrackIds: ReadonlyMap<number, ReadonlyArray<number>>;
}

export interface CustomDimensionRow {
  readonly trackId: number;
  readonly name: string;
  readonly intValue: bigint | null;
  readonly stringValue: string | null;
  readonly displayValue: string;
  readonly displayName: string | null;
}

export interface ProcessRootTrackRow {
  readonly upid: number;
  readonly trackId: number;
}

export async function loadCustomDimensions(
  engine: Engine,
): Promise<CustomDimensions> {
  const result = await engine.query(`
    SELECT
      t.id AS trackId,
      substr(a.key, length('custom_dimension.') + 1) AS name,
      a.int_value AS intValue,
      a.string_value AS stringValue,
      a.display_value AS displayValue,
      label.string_value AS displayName
    FROM track AS t
    JOIN args AS a ON a.arg_set_id = t.dimension_arg_set_id
    LEFT JOIN args AS label
      ON label.arg_set_id = t.source_arg_set_id
      AND label.key = 'custom_dimension_display_name.' ||
        substr(a.key, length('custom_dimension.') + 1)
    WHERE a.key GLOB 'custom_dimension.*'
  `);
  const rows: CustomDimensionRow[] = [];
  const it = result.iter({
    trackId: NUM,
    name: STR,
    intValue: LONG_NULL,
    stringValue: STR_NULL,
    displayValue: STR,
    displayName: STR_NULL,
  });
  for (; it.valid(); it.next()) {
    rows.push({
      trackId: it.trackId,
      name: it.name,
      intValue: it.intValue,
      stringValue: it.stringValue,
      displayValue: it.displayValue,
      displayName: it.displayName,
    });
  }
  // Most traces have no custom dimensions: skip the process lookup.
  if (rows.length === 0) return buildCustomDimensions([], []);

  const rootResult = await engine.query(`
    SELECT upid, id AS trackId
    FROM process_track
    WHERE extract_arg(source_arg_set_id, 'is_root_in_scope') = 1
    ORDER BY upid, id
  `);
  const roots: ProcessRootTrackRow[] = [];
  const rootIt = rootResult.iter({upid: NUM, trackId: NUM});
  for (; rootIt.valid(); rootIt.next()) {
    roots.push({upid: rootIt.upid, trackId: rootIt.trackId});
  }
  return buildCustomDimensions(rows, roots);
}

export function buildCustomDimensions(
  rows: Iterable<CustomDimensionRow>,
  processRoots: Iterable<ProcessRootTrackRow>,
): CustomDimensions {
  const byTrack = new Map<number, Map<string, CustomDimension>>();
  const distinctKeys = new Map<string, Set<string>>();
  for (const row of rows) {
    const key =
      row.intValue !== null ? `i:${row.intValue}` : `s:${row.stringValue}`;
    const label =
      row.displayName === null || row.displayName === ''
        ? `${row.name} ${row.displayValue}`
        : row.displayName;
    getOrCreate(byTrack, row.trackId, () => new Map()).set(row.name, {
      name: row.name,
      key,
      label,
    });
    getOrCreate(distinctKeys, row.name, () => new Set()).add(key);
  }
  const varyingNames = new Set<string>();
  for (const [name, keys] of distinctKeys) {
    if (keys.size > 1) varyingNames.add(name);
  }
  const processRootTrackIds = new Map<number, number[]>();
  for (const root of processRoots) {
    getOrCreate(processRootTrackIds, root.upid, () => []).push(root.trackId);
  }
  return {byTrack, varyingNames, processRootTrackIds};
}

// Returns the visible dimensions shared by every track in `trackIds`, sorted
// by name. A UI track backed by several trace processor tracks only shows a
// dimension if all of them agree on its value and label.
export function getCustomDimensions(
  dimensions: CustomDimensions,
  trackIds: ReadonlyArray<number>,
): CustomDimension[] {
  const [first, ...rest] = trackIds;
  if (first === undefined) return [];
  const candidates = dimensions.byTrack.get(first);
  if (candidates === undefined) return [];
  return Array.from(candidates.values())
    .filter(
      (dimension) =>
        dimensions.varyingNames.has(dimension.name) &&
        rest.every((id) => {
          const other = dimensions.byTrack.get(id)?.get(dimension.name);
          return (
            other?.key === dimension.key && other.label === dimension.label
          );
        }),
    )
    .sort((a, b) => a.name.localeCompare(b.name));
}

// Returns the visible dimensions describing the process as a whole.
export function getProcessCustomDimensions(
  dimensions: CustomDimensions,
  upid: number,
): CustomDimension[] {
  return getCustomDimensions(
    dimensions,
    dimensions.processRootTrackIds.get(upid) ?? [],
  );
}

// Returns the chip labels for `own` dimensions, omitting the ones already
// shown with the same value by an ancestor (`inherited`).
export function getCustomDimensionChips(
  own: ReadonlyArray<CustomDimension>,
  inherited: ReadonlyArray<CustomDimension> = [],
): string[] {
  return own
    .filter(
      (dimension) =>
        !inherited.some(
          (other) =>
            other.name === dimension.name && other.key === dimension.key,
        ),
    )
    .map((dimension) => dimension.label);
}
