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

import type {DiffRow} from './diff_rows';

export type DiffViewName =
  | 'classes'
  | 'strings'
  | 'arrays'
  | 'bitmaps'
  | 'dominators'
  | 'overview';

interface Snapshot {
  readonly gen: number;
  readonly rows: ReadonlyArray<DiffRow>;
}

const snapshots = new Map<DiffViewName, Snapshot>();
let nextGen = 1;

export function publishDiffRows(
  view: DiffViewName,
  rows: ReadonlyArray<DiffRow>,
): void {
  snapshots.set(view, {gen: nextGen++, rows});
}

export function clearDiffRows(view?: DiffViewName): void {
  if (view) snapshots.delete(view);
  else snapshots.clear();
}

export interface HeapdumpDiffDebugApi {
  rows(view: DiffViewName): ReadonlyArray<DiffRow> | null;
  gen(view: DiffViewName): number;
  views(): DiffViewName[];
}

declare global {
  interface Window {
    __heapdumpDiff?: HeapdumpDiffDebugApi;
  }
}

if (typeof window !== 'undefined') {
  window.__heapdumpDiff = {
    rows: (view) => snapshots.get(view)?.rows ?? null,
    gen: (view) => snapshots.get(view)?.gen ?? 0,
    views: () => Array.from(snapshots.keys()),
  };
}
