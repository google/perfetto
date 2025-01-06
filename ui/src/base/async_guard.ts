// Copyright (C) 2024 The Android Open Source Project
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

/**
 * AsyncGuard<T> ensures that a given asynchronous operation does not overlap
 * with itself.
 *
 * This class is useful in scenarios where you want to prevent concurrent
 * executions of the same async function. If the function is already in
 * progress, any subsequent calls to `run` will return the same promise,
 * ensuring no new execution starts until the ongoing one completes.
 *
 * - Guarantees single execution: Only one instance of the provided async
 *   function will execute at a time.
 * - Automatically resets: Once the function completes (either successfully
 *   or with an error), the guard resets and allows new executions.
 *
 * This class differs from AsyncLimiter in the fact that it has no queueing at
 * all (AsyncLimiter instead keeps a queue of max_depth=1 and keeps over-writing
 * the last task).
 *
 * Example:
 * ```typescript
 * const asyncTask = async () => {
 *   console.log("Task started...");
 *   await new Promise(resolve => setTimeout(resolve, 2000)); // Simulate work.
 *   console.log("Task finished!");
 *   return "Result";
 * };
 *
 * const guard = new AsyncGuard<string>();
 *
 * // Simultaneous calls
 * guard.run(asyncTask).then(console.log); // Logs "Task started..." and
 *                                         // "Task finished!" -> "Result"
 * guard.run(asyncTask).then(console.log); // Will not log "Task started..."
 *                                         // again, reuses the promise
 * ```
 */
export class AsyncGuard<T> {
  private pendingPromise?: Promise<T>;

  /**
   * Runs the provided async function, ensuring no overlap.
   * If a previous call is still pending, it returns the same promise.
   *
   * @param func - The async function to execute.
   * @returns A promise resolving to the function's result.
   */
  run(func: () => Promise<T>): Promise<T> {
    if (this.pendingPromise !== undefined) {
      return this.pendingPromise;
    }
    this.pendingPromise = func();
    this.pendingPromise.finally(() => {
      this.pendingPromise = undefined;
    });
    return this.pendingPromise;
  }
}
