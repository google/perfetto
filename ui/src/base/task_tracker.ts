// Copyright (C) 2025 The Android Open Source Project
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

import {defer} from './deferred';

interface TaskEntry {
  readonly label: string;
  readonly startTime: number;
}

export interface TaskInfo {
  readonly label: string;
  readonly elapsed: number;
}

/**
 * TaskTracker provides observability over async work in the application.
 *
 * It is decoupled from scheduling - it doesn't schedule or manage tasks itself,
 * it simply watches promises. Any async work (scheduled tasks, fetches,
 * arbitrary promises) can opt in by calling track().
 */
export class TaskTracker {
  private readonly pending = new Map<number, TaskEntry>();
  private readonly listeners = new Set<() => void>();
  private nextId = 0;

  /**
   * Register a promise to be tracked.
   *
   * @param promise The promise to track
   * @param label A label for display/debugging purposes (defaults to 'anonymous')
   * @returns The same promise (pass-through) so callers can still await it
   */
  track<T>(promise: Promise<T>, label = 'anonymous'): Promise<T> {
    const id = this.nextId++;
    const entry: TaskEntry = {
      label,
      startTime: performance.now(),
    };
    this.pending.set(id, entry);
    this.notify();

    // Attach cleanup via .finally(), but suppress the unhandled rejection
    // on this side chain. The original promise is returned so rejections
    // propagate correctly to the caller.
    promise
      .finally(() => {
        this.pending.delete(id);
        this.notify();
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

  /**
   * Returns a promise that resolves when all currently tracked work
   * (including work added while waiting) has completed.
   * Resolves immediately if already idle.
   */
  whenIdle(): Promise<void> {
    if (this.idle) {
      return Promise.resolve();
    }

    const deferred = defer<void>();
    const unsubscribe = this.subscribe(() => {
      if (this.idle) {
        unsubscribe();
        deferred.resolve();
      }
    });

    return deferred;
  }

  /**
   * Register a listener that fires whenever a task is added or completes.
   *
   * @param fn The listener function
   * @returns An unsubscribe function
   */
  subscribe(fn: () => void): () => void {
    this.listeners.add(fn);
    return () => {
      this.listeners.delete(fn);
    };
  }

  private notify(): void {
    for (const listener of this.listeners) {
      listener();
    }
  }
}

// Singleton instance
export const taskTracker = new TaskTracker();

// Expose on globalThis for Playwright testing
declare global {
  interface Window {
    __taskTracker: TaskTracker;
  }
}

if (typeof window !== 'undefined') {
  window.__taskTracker = taskTracker;
}
