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

/**
 * Defines a mapping between pixels and Milliseconds for the entire application.
 * Linearly scales time values from boundsMs to pixel values in boundsPx and
 * back.
 */
export class TimeScale {
  private startMs: Milliseconds;
  private endMs: Milliseconds;
  private startPx: Pixels;
  private endPx: Pixels;
  private slopeMsPerPx = 0;

  constructor(
      boundsMs: [Milliseconds, Milliseconds], boundsPx: [Pixels, Pixels]) {
    this.startMs = boundsMs[0];
    this.endMs = boundsMs[1];
    this.startPx = boundsPx[0];
    this.endPx = boundsPx[1];
    this.updateSlope();
  }

  private updateSlope() {
    this.slopeMsPerPx =
        (this.endMs - this.startMs) / (this.endPx - this.startPx);
  }

  msToPx(time: Milliseconds): Pixels {
    return this.startPx as number + (time - this.startMs) / this.slopeMsPerPx;
  }

  pxToMs(px: Pixels): Milliseconds {
    return this.startMs as number + (px - this.startPx) * this.slopeMsPerPx;
  }

  deltaPxToDurationMs(px: Pixels): Milliseconds {
    return px * this.slopeMsPerPx;
  }

  setLimitsMs(tStart: Milliseconds, tEnd: Milliseconds) {
    this.startMs = tStart;
    this.endMs = tEnd;
    this.updateSlope();
  }

  setLimitsPx(pxStart: Pixels, pxEnd: Pixels) {
    this.startPx = pxStart;
    this.endPx = pxEnd;
    this.updateSlope();
  }
}

// We are using enums because TypeScript does proper type checking for those,
// and disallows assigning a pixel value to a milliseconds value, even though
// they are numbers. Using types, this safeguard would not be here.
// See: https://stackoverflow.com/a/43832165

export enum Pixels {
}
export enum Milliseconds {
}
