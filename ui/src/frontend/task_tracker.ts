// Copyright (C) 2020 The Android Open Source Project
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

interface PromiseInfo {
  startTimeMs: number;
  message: string;
}

export class TaskTracker {
  private promisesSeen: number;
  private promisesRejected: number;
  private promisesFulfilled: number;
  private promiseInfo: Map<Promise<unknown>, PromiseInfo>;

  constructor() {
    this.promisesSeen = 0;
    this.promisesRejected = 0;
    this.promisesFulfilled = 0;
    this.promiseInfo = new Map();
  }

  trackPromise(promise: Promise<unknown>, message: string): void {
    this.promiseInfo.set(promise, {
      startTimeMs: new Date().getMilliseconds(),
      message,
    });
    this.promisesSeen += 1;
    promise
      .then(() => {
        this.promisesFulfilled += 1;
      })
      .catch(() => {
        this.promisesRejected += 1;
      })
      .finally(() => {
        this.promiseInfo.delete(promise);
      });
  }

  hasPendingTasks(): boolean {
    return this.promisesSeen > this.promisesFulfilled + this.promisesRejected;
  }

  progressMessage(): string | undefined {
    const {value} = this.promiseInfo.values().next();
    if (value === undefined) {
      return value;
    } else {
      const nowMs = new Date().getMilliseconds();
      const runtimeSeconds = Math.round((nowMs - value.startTimeMs) / 1000);
      return `${value.message} (${runtimeSeconds}s)`;
    }
  }
}

export const taskTracker = new TaskTracker();
