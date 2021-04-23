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
import {TimeSpan} from '../../common/time';
import {globals} from '../globals';
import {TimeScale} from '../time_scale';

export abstract class DragStrategy {
  constructor(protected timeScale: TimeScale) {}

  abstract onDrag(x: number): void;

  abstract onDragStart(x: number): void;

  protected updateGlobals(tStart: number, tEnd: number) {
    const vizTime = new TimeSpan(tStart, tEnd);
    globals.frontendLocalState.updateVisibleTime(vizTime);
    globals.rafScheduler.scheduleRedraw();
  }
}
