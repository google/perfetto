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

import type {ProcMemStat} from './proc_mem_stats';

export const MEMORY_PAGE_ROOT = '/memory';

export type MemoryTab = 'summary' | 'smaps' | 'heap';

export interface MemorySubpage {
  readonly upid?: number;
  readonly tab?: MemoryTab;
  readonly hdeSubpage?: string;
}

export function parseMemorySubpage(subpage?: string): MemorySubpage {
  if (subpage === undefined || subpage === '') {
    return {};
  }
  const parts = subpage.split('/').filter((x) => x !== '');
  const rawUpid = parts[0];
  if (rawUpid === undefined) {
    return {};
  }
  const upid = parseInt(rawUpid, 10);
  const rawTab = parts[1];
  let tab: MemoryTab = 'summary';
  let hdeSubpage: string | undefined;
  if (rawTab === 'smaps') {
    tab = 'smaps';
  } else if (rawTab === 'heap') {
    tab = 'heap';
    if (parts.length > 2) {
      hdeSubpage = parts.slice(2).join('/');
    }
  }
  return {
    upid: Number.isNaN(upid) ? Number.NaN : upid,
    tab,
    hdeSubpage,
  };
}

export function formatMemorySubpage(
  upid: number,
  tab: MemoryTab = 'summary',
  hdeSubpage?: string,
): string {
  if (tab === 'summary') {
    return `${upid}`;
  }
  if (tab === 'heap') {
    return hdeSubpage !== undefined && hdeSubpage !== ''
      ? `${upid}/heap/${hdeSubpage}`
      : `${upid}/heap`;
  }
  return `${upid}/${tab}`;
}

// Default scope when opening a process without an explicit tab in the URL.
// Processes with only Java heap dumps open directly in the Java Heap scope;
// processes with smaps or native profile data open in the cross-source Summary.
export function defaultTabForProc(proc: ProcMemStat): MemoryTab {
  if (
    proc.smapsSnapshots === 0 &&
    proc.nativeDumps === 0 &&
    proc.heapDumps > 0
  ) {
    return 'heap';
  }
  return 'summary';
}

// Coerces a requested tab to one that has data for `proc`, falling back cleanly
// when switching processes (e.g. from a process with smaps/heap dumps to one
// without).
export function resolveTabForProc(
  requested: MemoryTab,
  proc: ProcMemStat | undefined,
): MemoryTab {
  if (proc === undefined) return requested;
  if (requested === 'smaps' && proc.smapsSnapshots === 0) {
    return defaultTabForProc(proc);
  }
  if (requested === 'heap' && proc.heapDumps === 0) {
    return 'summary';
  }
  return requested;
}
