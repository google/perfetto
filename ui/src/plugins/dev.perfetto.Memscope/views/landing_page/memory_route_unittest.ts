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

import {
  defaultTabForProc,
  formatMemorySubpage,
  parseMemorySubpage,
  resolveTabForProc,
} from './memory_route';
import type {ProcMemStat} from './proc_mem_stats';

function makeProc(overrides: Partial<ProcMemStat> = {}): ProcMemStat {
  return {
    upid: 1,
    pid: 100,
    procName: 'com.example.app',
    heapDumps: 0,
    smapsSnapshots: 0,
    nativeDumps: 0,
    ...overrides,
  };
}

describe('memory_route', () => {
  test('parseMemorySubpage handles empty, summary, smaps, and heap routes', () => {
    expect(parseMemorySubpage(undefined)).toEqual({});
    expect(parseMemorySubpage('')).toEqual({});
    expect(parseMemorySubpage('/42')).toEqual({
      upid: 42,
      tab: 'summary',
      hdeSubpage: undefined,
    });
    expect(parseMemorySubpage('/42/smaps')).toEqual({
      upid: 42,
      tab: 'smaps',
      hdeSubpage: undefined,
    });
    expect(parseMemorySubpage('/42/heap')).toEqual({
      upid: 42,
      tab: 'heap',
      hdeSubpage: undefined,
    });
    expect(parseMemorySubpage('/42/heap/objects_java.lang.String')).toEqual({
      upid: 42,
      tab: 'heap',
      hdeSubpage: 'objects_java.lang.String',
    });
  });

  test('formatMemorySubpage round-trips summary, smaps, and heap subpages', () => {
    expect(formatMemorySubpage(7, 'summary')).toBe('7');
    expect(formatMemorySubpage(7, 'smaps')).toBe('7/smaps');
    expect(formatMemorySubpage(7, 'heap')).toBe('7/heap');
    expect(formatMemorySubpage(7, 'heap', 'classes')).toBe('7/heap/classes');
  });

  test('defaultTabForProc prefers heap for heap-only processes and summary otherwise', () => {
    expect(defaultTabForProc(makeProc({heapDumps: 1}))).toBe('heap');
    expect(defaultTabForProc(makeProc({heapDumps: 1, smapsSnapshots: 2}))).toBe(
      'summary',
    );
    expect(defaultTabForProc(makeProc({heapDumps: 1, nativeDumps: 3}))).toBe(
      'summary',
    );
    expect(defaultTabForProc(makeProc({smapsSnapshots: 5}))).toBe('summary');
  });

  test('resolveTabForProc falls back when a process lacks smaps or heap dumps', () => {
    const smapsOnly = makeProc({smapsSnapshots: 4});
    const heapOnly = makeProc({heapDumps: 2});
    const both = makeProc({smapsSnapshots: 4, heapDumps: 2});

    expect(resolveTabForProc('heap', smapsOnly)).toBe('summary');
    expect(resolveTabForProc('smaps', smapsOnly)).toBe('smaps');
    expect(resolveTabForProc('smaps', heapOnly)).toBe('heap');
    expect(resolveTabForProc('summary', heapOnly)).toBe('summary');
    expect(resolveTabForProc('heap', both)).toBe('heap');
    expect(resolveTabForProc('smaps', both)).toBe('smaps');
  });
});
