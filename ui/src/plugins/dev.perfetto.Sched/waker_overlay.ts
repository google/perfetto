// Copyright (C) 2025 The Android Open Source Project
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

import {AsyncLimiter} from '../../base/async_limiter';
import {canvasSave, drawDoubleHeadedArrow} from '../../base/canvas_utils';
import {Size2D} from '../../base/geom';
import {Duration, time} from '../../base/time';
import {TimeScale} from '../../base/time_scale';
import {drawVerticalLineAtTime} from '../../base/vertical_line_helper';
import {asSchedSqlId} from '../../components/sql_utils/core_types';
import {
  getSched,
  getSchedWakeupInfo,
  SchedWakeupInfo,
} from '../../components/sql_utils/sched';
import {Selection, TrackEventSelection} from '../../public/selection';
import {Trace} from '../../public/trace';
import {Overlay, TrackBounds} from '../../public/track';
import {CPU_SLICE_URI_PREFIX, uriForSchedTrack} from './common';

const MARGIN = 3;
const DIAMOND_SIZE = 8;
const ARROW_HEIGHT = 12;

export class WakerOverlay implements Overlay {
  private readonly limiter = new AsyncLimiter();
  private readonly trace: Trace;
  private readonly wakeupCache = new WeakMap<Selection, SchedWakeupInfo>();
  private cachedSelection?: Selection;

  constructor(trace: Trace) {
    this.trace = trace;
  }

  render(
    canvasCtx: CanvasRenderingContext2D,
    timescale: TimeScale,
    size: Size2D,
    renderedTracks: ReadonlyArray<TrackBounds>,
  ): void {
    const selection = this.trace.selection.selection;

    // Get out if selection is not a CPU slice.
    if (!this.cpuSliceTrackSelected(selection)) {
      this.cachedSelection = undefined;
      return;
    }

    // Compare the current selection with the cached one to determine if it has
    // changed and we need to start loading new wakeup info.
    if (this.cachedSelection !== selection) {
      this.cachedSelection = selection;
      this.limiter.schedule(async () => {
        const wakeupInfo = await this.loadWakeupInfo(selection);
        if (!wakeupInfo) return;
        this.wakeupCache.set(selection, wakeupInfo);
      });
    }

    // Check if we have the wakeup info cached, get out if not.
    const wakeup = this.wakeupCache.get(selection);
    if (!wakeup || !wakeup.wakeupTs) {
      return;
    }

    // Draw the vertical line at the wakeup timestamp
    this.drawWakeupLine(canvasCtx, timescale, size.height, wakeup.wakeupTs);

    // Draw the marker on the waker CPU track
    if (wakeup.wakerCpu !== undefined) {
      this.drawWakerMarker(
        canvasCtx,
        timescale,
        renderedTracks,
        wakeup.wakeupTs,
        wakeup.wakerCpu,
      );
    }

    this.drawLatencyArrow(
      canvasCtx,
      timescale,
      renderedTracks,
      wakeup.wakeupTs,
      selection.trackUri,
      selection.ts,
    );
  }

  private cpuSliceTrackSelected(
    selection: Selection,
  ): selection is TrackEventSelection {
    return (
      selection.kind === 'track_event' &&
      selection.trackUri.startsWith(CPU_SLICE_URI_PREFIX)
    );
  }

  private async loadWakeupInfo(
    selection: TrackEventSelection,
  ): Promise<SchedWakeupInfo | undefined> {
    const sched = await getSched(
      this.trace.engine,
      asSchedSqlId(selection.eventId),
    );
    if (!sched) return undefined;
    const cache = await getSchedWakeupInfo(this.trace.engine, sched);
    return cache;
  }

  private drawWakeupLine(
    canvasCtx: CanvasRenderingContext2D,
    timescale: TimeScale,
    height: number,
    wakeupTs: time,
  ): void {
    drawVerticalLineAtTime(canvasCtx, timescale, wakeupTs, height, `black`);
  }

  private drawWakerMarker(
    canvasCtx: CanvasRenderingContext2D,
    timescale: TimeScale,
    renderedTracks: ReadonlyArray<TrackBounds>,
    wakeupTs: time,
    wakerCpu: number,
  ): void {
    const wakerCpuTrackUri = uriForSchedTrack(wakerCpu);
    const wakerTrack = renderedTracks.find(
      (track) => wakerCpuTrackUri === track.node.uri,
    );

    if (!wakerTrack) return;

    const bounds = wakerTrack.verticalBounds;
    const trackHeight = bounds.bottom - bounds.top;
    const rectHeight = trackHeight - 2 * MARGIN;
    const wakeupPosPx = Math.floor(timescale.timeToPx(wakeupTs));

    using _ = canvasSave(canvasCtx);
    canvasCtx.translate(0, bounds.top);
    canvasCtx.beginPath();
    const yCenter = MARGIN + rectHeight / 2;
    canvasCtx.moveTo(wakeupPosPx, yCenter + DIAMOND_SIZE);
    canvasCtx.fillStyle = 'black';
    canvasCtx.lineTo(wakeupPosPx + DIAMOND_SIZE * 0.75, yCenter);
    canvasCtx.lineTo(wakeupPosPx, yCenter - DIAMOND_SIZE);
    canvasCtx.lineTo(wakeupPosPx - DIAMOND_SIZE * 0.75, yCenter);
    canvasCtx.fill();
    canvasCtx.closePath();
  }

  private drawLatencyArrow(
    canvasCtx: CanvasRenderingContext2D,
    timescale: TimeScale,
    renderedTracks: ReadonlyArray<TrackBounds>,
    wakeupTs: time,
    wakedTrackUri: string,
    wakedSliceTs: time,
  ): void {
    const wakedTrack = renderedTracks.find(
      (track) => wakedTrackUri === track.node.uri,
    );

    if (!wakedTrack) return;

    const bounds = wakedTrack.verticalBounds;
    const trackHeight = bounds.bottom - bounds.top;
    const rectHeight = trackHeight - 2 * MARGIN;
    const wakeupPosPx = timescale.timeToPx(wakeupTs);
    const wakedSliceStartPx = timescale.timeToPx(wakedSliceTs);
    const latencyWidthPx = wakedSliceStartPx - wakeupPosPx;

    using _ = canvasSave(canvasCtx);
    canvasCtx.translate(0, bounds.top);

    // Draw the double-headed arrow
    drawDoubleHeadedArrow(
      canvasCtx,
      wakeupPosPx,
      MARGIN + rectHeight,
      latencyWidthPx,
      latencyWidthPx >= 20, // Only draw arrow heads if width is sufficient
    );

    // Draw latency text if space permits
    const latency = wakedSliceTs - wakeupTs;
    const displayText = Duration.humanise(latency);
    const measured = canvasCtx.measureText(displayText);
    if (latencyWidthPx >= measured.width + 2) {
      const textX = wakeupPosPx + latencyWidthPx / 2;
      const textY = MARGIN + rectHeight - 1;
      const textBgY = MARGIN + rectHeight - ARROW_HEIGHT;

      // Semi-transparent background for text
      canvasCtx.fillStyle = 'rgba(255,255,255,0.7)';
      canvasCtx.fillRect(
        textX - measured.width / 2 - 1,
        textBgY,
        measured.width + 2,
        ARROW_HEIGHT - 1, // Height adjusted to fit within arrow bounds
      );

      // Latency text
      canvasCtx.textBaseline = 'bottom';
      canvasCtx.fillStyle = 'black';
      canvasCtx.textAlign = 'center';
      canvasCtx.fillText(displayText, textX, textY);
    }
  }
}
