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

// SQL column for heap graph flamegraph queries: '1' if the class named by
// `nameExpr` is potentially merged, else '0'. Class tree nodes only carry the
// (deobfuscated) class name, so this matches heap_graph_class by name.
export function mergedClassColumnSql(nameExpr: string): string {
  return `CAST(${nameExpr} IN (
      SELECT coalesce(deobfuscated_name, name)
      FROM heap_graph_class
      WHERE potentially_merged
    ) AS TEXT) AS ${MERGED_CLASS_PROPERTY}`;
}
