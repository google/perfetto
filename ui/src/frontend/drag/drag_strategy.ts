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
import {HighPrecisionTime} from '../../base/high_precision_time';
import {HighPrecisionTimeSpan} from '../../base/high_precision_time_span';
import {TimeScale} from '../../base/time_scale';

export type DragStrategyUpdateTimeFn = (ts: HighPrecisionTimeSpan) => void;

export abstract class DragStrategy {
  constructor(
    protected map: TimeScale,
    private updateVizTime: DragStrategyUpdateTimeFn,
  ) {}

  abstract onDrag(x: number): void;

  abstract onDragStart(x: number): void;

  protected updateGlobals(tStart: HighPrecisionTime, tEnd: HighPrecisionTime) {
    const vizTime = new HighPrecisionTimeSpan(
      tStart,
      tEnd.sub(tStart).toNumber(),
    );
    this.updateVizTime(vizTime);
  }
}
