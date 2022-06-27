// Copyright (C) 2021 The Android Open Source Project
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
import {TimeScale} from '../time_scale';

import {DragStrategy} from './drag_strategy';

export class BorderDragStrategy extends DragStrategy {
  private moveStart = false;

  constructor(timeScale: TimeScale, private pixelBounds: [number, number]) {
    super(timeScale);
  }

  onDrag(x: number) {
    let tStart =
        this.timeScale.pxToTime(this.moveStart ? x : this.pixelBounds[0]);
    let tEnd =
        this.timeScale.pxToTime(!this.moveStart ? x : this.pixelBounds[1]);
    if (tStart > tEnd) {
      this.moveStart = !this.moveStart;
      [tEnd, tStart] = [tStart, tEnd];
    }
    super.updateGlobals(tStart, tEnd);
    this.pixelBounds =
        [this.timeScale.timeToPx(tStart), this.timeScale.timeToPx(tEnd)];
  }

  onDragStart(x: number) {
    this.moveStart =
        Math.abs(x - this.pixelBounds[0]) < Math.abs(x - this.pixelBounds[1]);
  }
}
