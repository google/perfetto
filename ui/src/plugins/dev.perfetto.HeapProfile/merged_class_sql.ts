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

// Heap graph flamegraph query pieces for flagging potentially merged classes
// (see heap_graph_class.potentially_merged). Pair with mergedClassNodeIcon()
// and mergedClassNodeTooltipRenderer() from
// components/merged_class_flamegraph_hooks.

// Name of the flamegraph node property that holds the flag.
export const MERGED_CLASS_PROPERTY = 'potentially_merged';

// Unaggregatable property definition for heap graph flamegraph metrics. Hidden
// from the tooltip's property rows because the warning callout explains it.
export const MERGED_CLASS_FLAMEGRAPH_PROPERTY = {
  name: MERGED_CLASS_PROPERTY,
  displayName: 'Potentially merged',
  isVisible: () => false,
};

// SQL query for the names (deobfuscated where possible) of the potentially
// merged classes in the heap dump (`upid`, `ts`). Each dump has its own
// heap_graph_class rows but the table has no dump columns, so the dump's
// classes are those of its objects plus their superclasses (which may have no
// instances of their own, e.g. an abstract class that others were merged
// into).
export function mergedClassNamesSql(upid: number, ts: bigint): string {
  return `
    WITH RECURSIVE dump_class(id) AS (
      SELECT DISTINCT type_id
      FROM heap_graph_object
      WHERE upid = ${upid} AND graph_sample_ts = ${ts}
      UNION
      SELECT c.superclass_id
      FROM heap_graph_class c
      JOIN dump_class d ON c.id = d.id
      WHERE c.superclass_id IS NOT NULL
    )
    SELECT DISTINCT coalesce(c.deobfuscated_name, c.name) AS name
    FROM heap_graph_class c
    JOIN dump_class USING (id)
    WHERE c.potentially_merged
  `;
}

// SQL column for heap graph flamegraph queries over the heap dump (`upid`,
// `ts`): '1' if the class named by `nameExpr` is potentially merged, else '0'.
// Class tree nodes only carry the (deobfuscated) class name, so this matches
// the dump's classes by name.
export function mergedClassColumnSql(
  nameExpr: string,
  upid: number,
  ts: bigint,
): string {
  return `CAST(${nameExpr} IN (${mergedClassNamesSql(upid, ts)}) AS TEXT)
    AS ${MERGED_CLASS_PROPERTY}`;
}
