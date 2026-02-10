// Copyright (C) 2026 The Android Open Source Project
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

import {Size2D} from '../../base/geom';
import {TimeScale} from '../../base/time_scale';
import {Trace} from '../../public/trace';
import {Overlay, TrackBounds} from '../../public/track';
import {ArrowConnection, ArrowVisualiser} from './arrow_visualiser';

export class LifecycleOverlay implements Overlay {
  private readonly arrowVisualiser: ArrowVisualiser;
  private connections: ArrowConnection[] = [];

  constructor(trace: Trace) {
    this.arrowVisualiser = new ArrowVisualiser(trace);
  }

  update(connections: ArrowConnection[]) {
    this.connections = connections;
  }

  render(
    ctx: CanvasRenderingContext2D,
    ts: TimeScale,
    _size: Size2D,
    tracks: ReadonlyArray<TrackBounds>,
  ): void {
    if (this.connections.length > 0) {
      this.arrowVisualiser.draw(ctx, ts, tracks, this.connections);
    }
  }
}
