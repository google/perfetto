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

// Cross-plugin data passing for flamegraph → Heapdump Explorer navigation.
//
// When the user clicks "Open in Heapdump Explorer" on a flamegraph node, the
// HeapProfile plugin stores the selected node's path hashes here. The
// Heapdump Explorer plugin consumes them when rendering the
// flamegraph-objects view. This avoids using temporary SQL tables as a
// cross-plugin API.

export interface FlamegraphHeapdumpSelection {
  /** Comma-separated path hashes from the flamegraph node's path_hash_stable. */
  pathHashes: string;
  /** Whether the hashes come from the dominator tree or shortest-path tree. */
  isDominator: boolean;
  /** The flamegraph node name (class name) for display. */
  name?: string;
}

let pending: FlamegraphHeapdumpSelection | undefined;

export function setFlamegraphHeapdumpSelection(
  sel: FlamegraphHeapdumpSelection,
): void {
  pending = sel;
}

export function consumeFlamegraphHeapdumpSelection():
  | FlamegraphHeapdumpSelection
  | undefined {
  const s = pending;
  pending = undefined;
  return s;
}
