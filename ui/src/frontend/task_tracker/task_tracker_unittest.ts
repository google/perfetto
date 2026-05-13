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

import {defer} from '../../base/deferred';
import {TaskTrackerImpl} from './task_tracker';

describe('TaskTracker', () => {
  let tracker: TaskTrackerImpl;

  beforeEach(() => {
    tracker = new TaskTrackerImpl();
  });

  test('starts idle with no tasks', () => {
    expect(tracker.idle).toBe(true);
    expect(tracker.size).toBe(0);
    expect(tracker.tasks).toEqual([]);
  });

  test('track() returns the same promise', async () => {
    const original = Promise.resolve(42);
    const tracked = tracker.track(original, 'test');
    expect(await tracked).toBe(42);
  });

  test('tracking a promise marks tracker as not idle', () => {
    const deferred = defer<void>();
    tracker.track(deferred, 'test');
    expect(tracker.idle).toBe(false);
    expect(tracker.size).toBe(1);
  });

  test('resolving a tracked promise removes it', async () => {
    const deferred = defer<void>();
    tracker.track(deferred, 'test');
    expect(tracker.size).toBe(1);

    deferred.resolve();
    await deferred;
    // Wait for the .finally() microtask
    await Promise.resolve();

    expect(tracker.size).toBe(0);
    expect(tracker.idle).toBe(true);
  });

  test('rejecting a tracked promise removes it', async () => {
    const deferred = defer<void>();
    const tracked = tracker.track(deferred, 'test');
    expect(tracker.size).toBe(1);

    deferred.reject(new Error('test error'));
    await expect(tracked).rejects.toThrow('test error');
    // Wait for the .finally() microtask
    await Promise.resolve();

    expect(tracker.size).toBe(0);
    expect(tracker.idle).toBe(true);
  });

  test('tasks snapshot includes label and elapsed time', () => {
    const deferred = defer<void>();
    tracker.track(deferred, 'my task');

    const tasks = tracker.tasks;
    expect(tasks).toHaveLength(1);
    expect(tasks[0].label).toBe('my task');
    expect(tasks[0].elapsed).toBeGreaterThanOrEqual(0);
  });

  test('uses anonymous label by default', () => {
    const deferred = defer<void>();
    tracker.track(deferred);

    expect(tracker.tasks[0].label).toBe('anonymous');
  });

  test('can track multiple promises', () => {
    const d1 = defer<void>();
    const d2 = defer<void>();
    const d3 = defer<void>();

    tracker.track(d1, 'task1');
    tracker.track(d2, 'task2');
    tracker.track(d3, 'task3');

    expect(tracker.size).toBe(3);
    expect(tracker.tasks.map((t) => t.label)).toEqual([
      'task1',
      'task2',
      'task3',
    ]);
  });
});
