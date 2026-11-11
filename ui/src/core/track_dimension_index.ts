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

import {assertTrue} from '../base/assert';
import type {Track} from '../public/track';
import type {TrackNode} from '../public/workspace';
import type {Engine} from '../trace_processor/engine';
import {LONG_NULL, NUM, STR, STR_NULL} from '../trace_processor/query_result';

interface IndexedDimension {
  readonly name: string;
  readonly key: string;
  readonly label: string;
}

type LookupTrack = (uri: string) => Track | undefined;

// Derived trace data, shared by every workspace and never stored on its nodes.
export class TrackDimensionIndex {
  private readonly byTrack = new Map<number, Map<string, IndexedDimension>>();
  private readonly visibleNames = new Set<string>();
  private initialization?: Promise<void>;

  constructor(private readonly engine: Pick<Engine, 'query'>) {}

  initialize(): Promise<void> {
    return (this.initialization ??= this.load());
  }

  private async load(): Promise<void> {
    const result = await this.engine.query(`
      SELECT
        t.id AS trackId,
        substr(a.key, length('custom_dimension.') + 1) AS name,
        a.value_type AS valueType,
        a.int_value AS intValue,
        a.string_value AS stringValue,
        label.string_value AS displayName,
        a.display_value AS displayValue
      FROM track AS t
      JOIN args AS a ON a.arg_set_id = t.dimension_arg_set_id
      LEFT JOIN args AS label
        ON label.arg_set_id = t.source_arg_set_id
        AND label.key = 'custom_dimension_display_name.' ||
          substr(a.key, length('custom_dimension.') + 1)
      WHERE a.key GLOB 'custom_dimension.*'
      ORDER BY t.id, a.key
    `);
    const iter = result.iter({
      trackId: NUM,
      name: STR,
      valueType: STR,
      intValue: LONG_NULL,
      stringValue: STR_NULL,
      displayName: STR_NULL,
      displayValue: STR,
    });
    const byTrack = new Map<number, Map<string, IndexedDimension>>();
    const values = new Map<string, Set<string>>();
    for (; iter.valid(); iter.next()) {
      assertTrue(
        (iter.valueType === 'int' &&
          iter.intValue !== null &&
          iter.stringValue === null) ||
          (iter.valueType === 'string' &&
            iter.stringValue !== null &&
            iter.intValue === null),
      );
      const key =
        iter.valueType === 'int'
          ? `i:${iter.intValue}`
          : `s:${iter.stringValue}`;
      const dimension = {
        name: iter.name,
        key,
        label:
          iter.displayName === null || iter.displayName === ''
            ? `${iter.name} ${iter.displayValue}`
            : iter.displayName,
      };
      let dimensions = byTrack.get(iter.trackId);
      if (dimensions === undefined) {
        dimensions = new Map();
        byTrack.set(iter.trackId, dimensions);
      }
      assertTrue(!dimensions.has(iter.name));
      dimensions.set(iter.name, dimension);
      let distinct = values.get(iter.name);
      if (distinct === undefined) {
        distinct = new Set();
        values.set(iter.name, distinct);
      }
      distinct.add(key);
    }
    for (const [id, dimensions] of byTrack) this.byTrack.set(id, dimensions);
    for (const [name, distinct] of values) {
      if (distinct.size > 1) this.visibleNames.add(name);
    }
  }

  getChips(
    node: TrackNode,
    lookup: LookupTrack,
    showHeadless = false,
  ): ReadonlyArray<string> {
    const sources = (n: TrackNode) => {
      const tags = n.uri === undefined ? undefined : lookup(n.uri)?.tags;
      return tags?.dimensionSourceTrackIds;
    };
    const inherited = new Map<string, Set<string>>();
    for (
      let parent = node.parent;
      parent !== undefined;
      parent = parent.parent
    ) {
      if (parent.headless && !showHeadless) continue;
      for (const dimension of this.agreedDimensions(sources(parent))) {
        let keys = inherited.get(dimension.name);
        if (keys === undefined) {
          keys = new Set();
          inherited.set(dimension.name, keys);
        }
        keys.add(dimension.key);
      }
    }
    return this.agreedDimensions(sources(node))
      .filter((dimension) => !inherited.get(dimension.name)?.has(dimension.key))
      .sort((a, b) => a.name.localeCompare(b.name))
      .map((dimension) => dimension.label);
  }

  private agreedDimensions(
    ids: ReadonlyArray<number> | undefined,
  ): IndexedDimension[] {
    if (ids === undefined || ids.length === 0) return [];
    const [first, ...rest] = Array.from(new Set(ids));
    if (first === undefined) return [];
    const dimensions = this.byTrack.get(first);
    if (dimensions === undefined) return [];
    return Array.from(dimensions.values()).filter(
      (dimension) =>
        this.visibleNames.has(dimension.name) &&
        rest.every((id) => {
          const other = this.byTrack.get(id)?.get(dimension.name);
          return (
            other?.key === dimension.key && other.label === dimension.label
          );
        }),
    );
  }
}
