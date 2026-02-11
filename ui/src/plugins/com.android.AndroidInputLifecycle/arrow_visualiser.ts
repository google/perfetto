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

import {drawBezierArrow} from '../../base/bezier_arrow';
import {time, Time} from '../../base/time';
import {TimeScale} from '../../base/time_scale';
import {Trace} from '../../public/trace';
import {TrackBounds} from '../../public/track';

/**
 * Represents a specific point in time and space (track + vertical depth)
 * where an arrow might start or end.
 */
export interface ArrowPoint {
  trackUri: string;
  ts: time;
  /**
   * Optional depth within the track (e.g., for slice tracks).
   * If undefined, the vertical center of the track is used.
   */
  depth?: number;
}

/**
 * A generic connection between two points to be drawn.
 */
export interface ArrowConnection {
  start: ArrowPoint;
  end: ArrowPoint;
}

export class ArrowVisualiser {
  private static readonly LINE_COLOR = `hsla(0, 100%, 60%, 1.00)`;
  private static readonly LINE_WIDTH = 2;

  constructor(private trace: Trace) {}

  draw(
    canvasCtx: CanvasRenderingContext2D,
    timescale: TimeScale,
    renderedTracks: ReadonlyArray<TrackBounds>,
    connections: ArrowConnection[],
  ): void {
    canvasCtx.strokeStyle = ArrowVisualiser.LINE_COLOR;
    canvasCtx.lineWidth = ArrowVisualiser.LINE_WIDTH;

    const trackBoundsMap = new Map<string, TrackBounds>();
    for (const track of renderedTracks) {
      if (track.node.uri) {
        trackBoundsMap.set(track.node.uri, track);
      }
    }

    for (const connection of connections) {
      const leftTrackBounds = trackBoundsMap.get(connection.start.trackUri);
      const rightTrackBounds = trackBoundsMap.get(connection.end.trackUri);

      // We can only draw if both source and dest tracks are currently rendered (visible)
      if (leftTrackBounds && rightTrackBounds) {
        const arrowStartX = timescale.timeToPx(
          Time.fromRaw(connection.start.ts),
        );
        const arrowEndX = timescale.timeToPx(Time.fromRaw(connection.end.ts));

        const arrowStartY = this.getYCoordinate(
          leftTrackBounds,
          connection.start.trackUri,
          connection.start.depth,
        );
        const arrowEndY = this.getYCoordinate(
          rightTrackBounds,
          connection.end.trackUri,
          connection.end.depth,
        );

        drawBezierArrow(
          canvasCtx,
          {x: arrowStartX, y: arrowStartY},
          {x: arrowEndX, y: arrowEndY},
        );
      }
    }
    canvasCtx.setLineDash([]);
  }

  /**
   * Calculates the Y coordinate.
   * If a depth is provided and the track supports slice layouts, it calculates the slice center.
   * Otherwise, it returns the vertical center of the track.
   */
  private getYCoordinate(
    trackBounds: TrackBounds,
    trackUri: string,
    depth?: number,
  ): number {
    const trackRect = trackBounds.verticalBounds;
    const trackInstance = this.trace.tracks.getTrack(trackUri);

    if (trackInstance && depth !== undefined) {
      // Attempt to get specific slice bounds if the track renderer supports it
      const sliceRectRaw =
        trackInstance.renderer.getSliceVerticalBounds?.(depth);
      if (sliceRectRaw) {
        return (
          trackRect.top +
          sliceRectRaw.top +
          (sliceRectRaw.bottom - sliceRectRaw.top) / 2
        );
      }
    }
    // Fallback: Track vertical center
    return trackRect.top + (trackRect.bottom - trackRect.top) / 2;
  }
}
