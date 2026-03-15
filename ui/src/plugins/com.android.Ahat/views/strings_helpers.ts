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

import type {StringListRow} from '../types';

export interface DuplicateGroup {
  value: string;
  count: number;
  wastedBytes: number;
  ids: number[];
}

/** Groups strings by value and identifies duplicates with wasted memory. */
export function computeDuplicates(rows: StringListRow[]): DuplicateGroup[] {
  const groups = new Map<
    string,
    {count: number; totalRetained: number; minRetained: number; ids: number[]}
  >();
  for (const r of rows) {
    const existing = groups.get(r.value);
    if (existing) {
      existing.count++;
      existing.totalRetained += r.retainedSize;
      existing.minRetained = Math.min(existing.minRetained, r.retainedSize);
      existing.ids.push(r.id);
    } else {
      groups.set(r.value, {
        count: 1,
        totalRetained: r.retainedSize,
        minRetained: r.retainedSize,
        ids: [r.id],
      });
    }
  }
  const result: DuplicateGroup[] = [];
  for (const [value, g] of groups) {
    if (g.count < 2) continue;
    result.push({
      value,
      count: g.count,
      wastedBytes: g.totalRetained - g.minRetained,
      ids: g.ids,
    });
  }
  result.sort((a, b) => b.wastedBytes - a.wastedBytes);
  return result;
}
