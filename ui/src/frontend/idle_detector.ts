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

import {defer} from '../base/deferred';
import {raf} from '../core/raf_scheduler';
import {AppImpl} from '../core/app_impl';

/**
 * This class is exposed by index.ts as window.waitForPerfettoIdle() and is used
 * by tests, to detect when we reach quiescence.
 */

const IDLE_HYSTERESIS_MS = 100;
const TIMEOUT_MS = 30_000;

export class IdleDetector {
  private promise = defer<void>();
  private deadline = performance.now() + TIMEOUT_MS;
  private idleSince?: number;
  private idleHysteresisMs = IDLE_HYSTERESIS_MS;

  waitForPerfettoIdle(idleHysteresisMs = IDLE_HYSTERESIS_MS): Promise<void> {
    this.idleSince = undefined;
    this.idleHysteresisMs = idleHysteresisMs;
    this.scheduleNextTask();
    return this.promise;
  }

  private onIdleCallback() {
    const now = performance.now();
    if (now > this.deadline) {
      this.promise.reject(
        `Didn't reach idle within ${TIMEOUT_MS} ms, giving up` +
          ` ${this.idleIndicators()}`,
      );
      return;
    }
    if (this.idleIndicators().every((x) => x)) {
      this.idleSince = this.idleSince ?? now;
      const idleDur = now - this.idleSince;
      if (idleDur >= this.idleHysteresisMs) {
        // We have been idle for more than the threshold, success.
        this.promise.resolve();
        return;
      }
      // We are idle, but not for long enough. keep waiting
      this.scheduleNextTask();
      return;
    }
    // Not idle, reset and repeat.
    this.idleSince = undefined;
    this.scheduleNextTask();
  }

  private scheduleNextTask() {
    requestIdleCallback(() => this.onIdleCallback());
  }

  private idleIndicators() {
    const reqsPending = AppImpl.instance.trace?.engine.numRequestsPending ?? 0;
    return [
      reqsPending === 0,
      !raf.hasPendingRedraws,
      !document.getAnimations().some((a) => a.playState === 'running'),
      document.querySelector('.progress.progress-anim') == null,
      document.querySelector('.omnibox.message-mode') == null,
    ];
  }
}
