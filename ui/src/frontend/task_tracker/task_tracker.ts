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
  TaskTracker as TaskTrackerInterface,
  TaskInfo,
} from '../../public/task_tracker';

interface TaskEntry {
  readonly label: string;
  readonly startTime: number;
}

/**
 * TaskTracker provides observability over async work in the application.
 *
 * It is decoupled from scheduling - it doesn't schedule or manage tasks itself,
 * it simply watches promises. Any async work (scheduled tasks, fetches,
 * arbitrary promises) can opt in by calling track().
 */
export class TaskTrackerImpl implements TaskTrackerInterface {
  private readonly pending = new Set<TaskEntry>();

  /**
   * Register a promise to be tracked.
   *
   * @param promise The promise to track
   * @param label A label for display/debugging purposes (defaults to 'anonymous')
   * @returns The same promise (pass-through) so callers can still await it
   */
  track<T>(promise: Promise<T>, label = 'anonymous'): Promise<T> {
    const entry: TaskEntry = {
      label,
      startTime: performance.now(),
    };
    this.pending.add(entry);

    // Attach cleanup via .finally(), but suppress the unhandled rejection
    // on this side chain. The original promise is returned so rejections
    // propagate correctly to the caller.
    promise
      .finally(() => {
        this.pending.delete(entry);
      })
      .catch(() => {});

    return promise;
  }

  /**
   * Number of currently in-flight tracked promises.
   */
  get size(): number {
    return this.pending.size;
  }

  /**
   * True when nothing is being tracked.
   */
  get idle(): boolean {
    return this.pending.size === 0;
  }

  /**
   * Returns true if there are pending tasks.
   * Convenience method for idle detection.
   */
  hasPendingTasks(): boolean {
    return this.pending.size > 0;
  }

  /**
   * Snapshot of all in-flight tasks with their label and elapsed time.
   */
  get tasks(): TaskInfo[] {
    const now = performance.now();
    return Array.from(this.pending.values()).map((entry) => ({
      label: entry.label,
      elapsed: now - entry.startTime,
    }));
  }
}
