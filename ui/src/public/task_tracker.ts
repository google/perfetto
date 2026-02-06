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
export interface TaskTracker {
  /**
   * Register a promise to be tracked.
   *
   * @param promise The promise to track
   * @param label A label for display/debugging purposes (defaults to 'anonymous')
   * @returns The same promise (pass-through) so callers can still await it
   */
  track<T>(promise: Promise<T>, label?: string): Promise<T>;

  /**
   * Number of currently in-flight tracked promises.
   */
  readonly size: number;

  /**
   * True when nothing is being tracked.
   */
  readonly idle: boolean;

  /**
   * Returns true if there are pending tasks.
   */
  hasPendingTasks(): boolean;

  /**
   * Snapshot of all in-flight tasks with their label and elapsed time.
   */
  readonly tasks: TaskInfo[];
}
