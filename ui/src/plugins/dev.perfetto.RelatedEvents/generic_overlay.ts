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
import {drawRelatedEvents} from './arrow_visualiser';
import {RelatedEventData} from './interface';

export class GenericRelatedEventsOverlay implements Overlay {
  private data: RelatedEventData = {events: [], relations: []};

  constructor(public trace: Trace) {}

  update(data: RelatedEventData) {
    this.data = data;
  }

  render(
    ctx: CanvasRenderingContext2D,
    ts: TimeScale,
    _size: Size2D,
    tracks: ReadonlyArray<TrackBounds>,
  ): void {
    const overlayData: RelatedEventData = {
      events: this.data.overlayEvents || [],
      relations: this.data.overlayRelations || [],
    };
    drawRelatedEvents(ctx, this.trace, ts, tracks, overlayData);
  }
}
