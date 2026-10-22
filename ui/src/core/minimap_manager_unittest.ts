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

import {Time, type time} from '../base/time';
import type {MinimapRow} from '../public/minimap';
import {MinimapManagerImpl} from './minimap_manager';

interface PendingLoad {
  readonly end: time;
  readonly resolve: (rows: MinimapRow[]) => void;
  readonly reject: (e: Error) => void;
}

// Returns a minimap whose provider's loads end when the test settles them.
function minimapWithPendingLoads() {
  const minimap = new MinimapManagerImpl();
  const loads: PendingLoad[] = [];
  minimap.registerContentProvider({
    priority: 1,
    getData: (span) =>
      new Promise((resolve, reject) => {
        loads.push({end: span.end.toTime(), resolve, reject});
      }),
  });
  return {minimap, loads};
}

// Lets pending promise callbacks run.
function flush(): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve));
}

test('runs loads one at a time, in order', async () => {
  const {minimap, loads} = minimapWithPendingLoads();
  const first = minimap.load(Time.ZERO, Time.fromSeconds(10));
  const second = minimap.load(Time.ZERO, Time.fromSeconds(20));
  await flush();
  expect(loads.map((l) => l.end)).toEqual([Time.fromSeconds(10)]);

  loads[0]!.resolve([]);
  await first;
  await flush();
  expect(loads.map((l) => l.end)).toEqual([
    Time.fromSeconds(10),
    Time.fromSeconds(20),
  ]);

  // The latest load's rows win.
  const rows: MinimapRow[] = [[]];
  loads[1]!.resolve(rows);
  await second;
  expect(minimap.getLoad()).toBe(rows);
});

test('runs a load after a failed one', async () => {
  const {minimap, loads} = minimapWithPendingLoads();
  const first = minimap.load(Time.ZERO, Time.fromSeconds(10));
  const second = minimap.load(Time.ZERO, Time.fromSeconds(20));
  await flush();

  loads[0]!.reject(new Error('Minimap failed'));
  await expect(first).rejects.toThrow('Minimap failed');
  await flush();
  expect(loads.length).toBe(2);

  const rows: MinimapRow[] = [[]];
  loads[1]!.resolve(rows);
  await second;
  expect(minimap.getLoad()).toBe(rows);
});
