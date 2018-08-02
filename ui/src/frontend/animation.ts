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
  private startMs = 0;
  private endMs = 0;
  private lastFrameMs = 0;
  private rafId = 0;

  constructor(private onAnimationStep: (timeSinceLastMs: number) => void) {}

  start(durationMs: number) {
    const nowMs = performance.now();

    // If the animation is already happening, just update its end time.
    if (nowMs <= this.endMs) {
      this.endMs = nowMs + durationMs;
      return;
    }
    this.lastFrameMs = 0;
    this.startMs = nowMs;
    this.endMs = nowMs + durationMs;
    this.rafId = requestAnimationFrame(this.onAnimationFrame.bind(this));
  }

  stop() {
    this.endMs = 0;
    cancelAnimationFrame(this.rafId);
  }

  get startTimeMs(): number {
    return this.startMs;
  }

  private onAnimationFrame(nowMs: number) {
    if (nowMs < this.endMs) {
      this.rafId = requestAnimationFrame(this.onAnimationFrame.bind(this));
    }
    this.onAnimationStep(nowMs - (this.lastFrameMs || nowMs));
    this.lastFrameMs = nowMs;
  }
}
