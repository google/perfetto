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
import {CanvasColors} from '../../public/canvas_colors';
import {Trace} from '../../public/trace';
import {Overlay, TrackBounds} from '../../public/track';
import {Dataset} from '../../trace_processor/dataset';
import {RelationVisualiser} from '../dev.perfetto.RelatedEvents/relation_visualiser/relation_visualiser';

export class ChannelRelationOverlay implements Overlay {
  private eventRelationRenderer: RelationVisualiser;
  private isActive = false;
  private currentDataset: Dataset | undefined;
  private currentChannel: string | undefined;

  constructor(trace: Trace) {
    this.eventRelationRenderer = new RelationVisualiser(trace);
  }

  updateOverlayData(
    active: boolean,
    channel?: string,
    dataset?: Dataset,
  ): void {
    const wasActive = this.isActive;
    if (!active || !channel || !dataset) {
      this.isActive = false;
      // Clear data only if it was active to avoid unnecessary churn
      if (wasActive) {
        this.currentDataset = undefined;
        this.currentChannel = undefined;
      }
    } else {
      this.isActive = true;
      // Update if channel or dataset instance has changed
      if (this.currentChannel !== channel || this.currentDataset !== dataset) {
        this.currentDataset = dataset;
        this.currentChannel = channel;
      }
    }
  }

  async render(
    canvasCtx: CanvasRenderingContext2D,
    timescale: TimeScale,
    _size: Size2D,
    renderedTracks: ReadonlyArray<TrackBounds>,
    _colors: CanvasColors,
  ): Promise<void> {
    if (!this.isActive || !this.currentChannel || !this.currentDataset) {
      return;
    }

    await this.eventRelationRenderer.drawRelations(
      canvasCtx,
      timescale,
      renderedTracks,
      this.currentDataset,
    );
  }
}
