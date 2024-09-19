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
import {TimeScale} from '../../base/time_scale';
import {DragStrategy} from './drag_strategy';

export class BorderDragStrategy extends DragStrategy {
  private moveStart = false;

  constructor(
    map: TimeScale,
    private pixelBounds: [number, number],
  ) {
    super(map);
  }

  onDrag(x: number) {
    const moveStartPx = this.moveStart ? x : this.pixelBounds[0];
    const moveEndPx = !this.moveStart ? x : this.pixelBounds[1];
    const tStart = this.map.pxToHpTime(Math.min(moveStartPx, moveEndPx));
    const tEnd = this.map.pxToHpTime(Math.max(moveStartPx, moveEndPx));
    if (moveStartPx > moveEndPx) {
      this.moveStart = !this.moveStart;
    }
    super.updateGlobals(tStart, tEnd);
    this.pixelBounds = [this.map.hpTimeToPx(tStart), this.map.hpTimeToPx(tEnd)];
  }

  onDragStart(x: number) {
    this.moveStart =
      Math.abs(x - this.pixelBounds[0]) < Math.abs(x - this.pixelBounds[1]);
  }
}
