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

type Priority = 'user-blocking' | 'user-visible' | 'background';

// Type declaration for the Scheduler API (not yet in TypeScript's lib.dom.d.ts)
declare global {
  interface Scheduler {
    yield(): Promise<void>;
    postTask(
      callback: (args: void) => void,
      options?: {priority?: Priority},
    ): Promise<void>;
  }
  // eslint-disable-next-line no-var
  var scheduler: Scheduler | undefined;
}

// Polyfill for scheduler.postTask()
export function postTask(
  callback: (args: void) => void,
  options?: {priority?: Priority},
): void {
  if (globalThis.scheduler?.postTask) {
    globalThis.scheduler.postTask(callback, options);
  } else {
    setTimeout(() => callback(), 0);
  }
}

// Polyfill for scheduler.yield()
export function yieldTask(): Promise<void> {
  if (globalThis.scheduler?.yield) {
    return globalThis.scheduler.yield();
  } else {
    return new Promise<void>((r) => setTimeout(() => r(), 0));
  }
}

export interface ChunkedTaskContext {
  readonly shouldYield: () => boolean;
  readonly yield: () => Promise<void>;
}

export interface ChunkedTaskOptions {
  readonly priority?: Priority;
  readonly workBudgetMs?: number;
}

// 4ms is a reasonable default budget
const DEFAULT_WORK_BUDGET_MS = 4;

/**
 * Returns a promise that resolves in a new task with the configured priority.
 * It returns a task context that can be used to cooperatively yield back to the
 * event loop after a certain work budget (in ms) has been exhausted.
 *
 * This helps create long-running tasks that do not block the main thread for
 * too long or hold up frames for too long.
 *
 * Uses the Scheduler API if available, otherwise falls back to setTimeout().
 *
 * The default priority is 'user-visible' and the default work budget is 4ms.
 *
 * Usage:
 * const task =  await deferChunkedTask({priority: 'user-visible', workBudgetMs: 5});
 * // Now running in a new task...
 * for (let i = 0; i < bigNumber; i++) {
 *   // do work...
 *   if (task.shouldYield()) {
 *     await task.yield();
 *   }
 * }
 */
export async function deferChunkedTask(
  opts: ChunkedTaskOptions = {},
): Promise<ChunkedTaskContext> {
  const {priority, workBudgetMs = DEFAULT_WORK_BUDGET_MS} = opts;

  return await new Promise<ChunkedTaskContext>((res) => {
    postTask(
      () => {
        res(createChunkedTaskContext(workBudgetMs));
      },
      {priority},
    );
  });
}

function createChunkedTaskContext(workBudgetMs: number): ChunkedTaskContext {
  let deadline = performance.now() + workBudgetMs;
  return {
    shouldYield: () => {
      const timeRemaining = deadline - performance.now();
      return timeRemaining <= 0;
    },
    yield: async () => {
      await yieldTask();
      // Reset the deadline after yielding
      deadline = performance.now() + workBudgetMs;
    },
  };
}
