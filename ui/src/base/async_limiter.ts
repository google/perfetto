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

import {Deferred, defer} from './deferred';

type Callback = () => Promise<void>;

interface Task {
  deferred: Deferred<void>;
  work: Callback;
}

/**
 * A tiny task queue management utility that ensures async tasks are not
 * executed concurrently.
 *
 * If a task is run while a previous one is still running, it is enqueued and
 * run after the first task completes.
 *
 * If multiple tasks are enqueued, only the latest task is run.
 */
export class AsyncLimiter {
  private readonly taskQueue: Task[] = [];
  private isRunning: boolean = false;

  /**
   * Schedule a task to be run.
   *
   * @param work An async function to schedule.
   * @returns A promise that resolves when either the task has finished
   * executing, or after the task has silently been discarded because a newer
   * task was scheduled.
   */
  schedule(work: Callback): Promise<void> {
    const deferred = defer<void>();
    this.taskQueue.push({work, deferred});

    if (!this.isRunning) {
      this.isRunning = true;
      this.runTaskQueue().finally(() => (this.isRunning = false));
    }

    return deferred;
  }

  private async runTaskQueue(): Promise<void> {
    let task: Task | undefined;

    while ((task = this.taskQueue.shift())) {
      if (this.taskQueue.length > 0) {
        task.deferred.resolve();
      } else {
        try {
          await task.work();
          task.deferred.resolve();
        } catch (e) {
          task.deferred.reject(e);
        }
      }
    }
  }
}
