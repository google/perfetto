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
import {DragStrategy, DragStrategyUpdateTimeFn} from './drag_strategy';

export class InnerDragStrategy extends DragStrategy {
  private dragStartPx = 0;

  constructor(
    timeScale: TimeScale,
    private pixelBounds: [number, number],
    updateVizTime: DragStrategyUpdateTimeFn,
  ) {
    super(timeScale, updateVizTime);
  }

  onDrag(x: number) {
    const move = x - this.dragStartPx;
    const tStart = this.map.pxToHpTime(this.pixelBounds[0] + move);
    const tEnd = this.map.pxToHpTime(this.pixelBounds[1] + move);
    super.updateGlobals(tStart, tEnd);
  }

  onDragStart(x: number) {
    this.dragStartPx = x;
  }
}
