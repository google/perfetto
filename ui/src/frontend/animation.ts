// Copyright (C) 2018 The Android Open Source Project
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

export class Animation {
  private running = false;
  private runningStartedMs = 0;
  private end = Infinity;
  private requestedAnimationFrame = 0;

  constructor(private onAnimationStep: (timeSinceLastMs: number) => void) {}

  start(durationMs?: number) {
    if (durationMs !== undefined) {
      this.end = Date.now() + durationMs;
    }
    this.run();
  }

  stop() {
    this.running = false;
    cancelAnimationFrame(this.requestedAnimationFrame);
  }

  getStartTimeMs(): number {
    return this.runningStartedMs;
  }

  private run() {
    if (this.running) {
      return;
    }
    let lastFrameTimeMs = 0;

    const raf = (timestampMs: number) => {
      if (!lastFrameTimeMs) {
        lastFrameTimeMs = timestampMs;
      }
      this.onAnimationStep(timestampMs - lastFrameTimeMs);
      lastFrameTimeMs = timestampMs;

      if (this.running) {
        if (Date.now() < this.end) {
          this.requestedAnimationFrame = requestAnimationFrame(raf);
        } else {
          this.running = false;
        }
      }
    };

    this.running = true;
    this.runningStartedMs = Date.now();

    requestAnimationFrame(raf);
  }
}