// Copyright (C) 2024 The Android Open Source Project
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

import {HighPrecisionTimeSpan} from '../base/high_precision_time_span';
import {time} from '../base/time';
import {Setting} from './settings';

export enum TimestampFormat {
  Timecode = 'timecode',
  TraceNs = 'traceNs',
  TraceNsLocale = 'traceNsLocale',
  Seconds = 'seconds',
  Milliseconds = 'milliseconds',
  Microseconds = 'microseconds',
  UTC = 'utc',
  CustomTimezone = 'customTimezone',
  TraceTz = 'traceTz',
}

export enum DurationPrecision {
  Full = 'full',
  HumanReadable = 'human_readable',
}

export interface PanIntoViewOptions {
  // Where to place the timestamp/span in the viewport.
  // nearest: pan the minimum amount to make it visible
  readonly align?: 'center' | 'nearest' | 'zoom';

  // Margin from edge as a fraction of the viewport (0.0 to 1.0)
  readonly margin?: number;

  // Whether to animate the pan/zoom operation.
  readonly animation?: 'ease-in-out' | 'step';
}

export interface PanInstantIntoViewOptions extends PanIntoViewOptions {
  // When zooming into an instant which has no inherent width, this value
  // defines what the new viewport width should be. If omitted, zoom is the same
  // as center for instants.
  readonly zoomWidth?: number;
}

export interface Timeline {
  /**
   * Pan the viewport by a specified amount of time.
   *
   * The viewport moves by the delta amount while maintaining the current zoom
   * level (duration). The result is clamped to trace boundaries.
   *
   * @param delta - The amount to pan in nanoseconds. Positive values pan
   * forward (right), negative values pan backward (left).
   *
   * @example
   * // Pan 100 nanoseconds to the right
   * timeline.pan(100);
   *
   * @example
   * // Pan 50 nanoseconds to the left
   * timeline.pan(-50);
   */
  pan(delta: number): void;

  /**
   * Zoom the viewport by a scaling factor around a center point.
   *
   * The viewport duration is multiplied by the factor, with the zoom centered
   * at the specified point. The result is clamped to minimum duration and trace
   * boundaries.
   *
   * @param factor - The scaling ratio. Values < 1 zoom in (reduce duration),
   * values > 1 zoom out (increase duration). E.g., 0.5 zooms in 2x, 2 zooms out
   * 2x.
   * @param centerPoint - Optional normalized position (0.0 to 1.0) where the
   * zoom is centered. 0.0 = left edge, 0.5 = center, 1.0 = right edge. Defaults
   * to 0.5 (center).
   *
   * @example
   * // Zoom in 2x around the center
   * timeline.zoom(0.5);
   *
   * @example
   * // Zoom out 3x around the left edge
   * timeline.zoom(3, 0);
   */
  zoom(factor: number, centerPoint?: number): void;

  /**
   * Pan (and optionally zoom) the viewport to make a timestamp visible.
   *
   * Depending on alignment mode:
   * - 'center': Centers the timestamp in the viewport
   * - 'nearest': Pans minimally to bring timestamp into view (default)
   * - 'zoom': Centers timestamp and optionally changes zoom level
   *
   * The viewport is clamped to trace boundaries while preserving duration unless
   * zooming.
   *
   * @param ts - The timestamp to bring into view.
   * @param options - Optional configuration:
   *   - align: How to position the timestamp ('center' | 'nearest' | 'zoom')
   *   - margin: Safe zone as fraction of viewport (e.g., 0.1 = 10% margin)
   *   - zoomWidth: For 'zoom' mode, the new viewport duration in nanoseconds
   *
   * @example
   * // Center timestamp 1500 in viewport
   * timeline.panIntoView(Time.fromRaw(1500n), {align: 'center'});
   *
   * @example
   * // Bring timestamp into view with 10% margin
   * timeline.panIntoView(Time.fromRaw(1500n), {
   *   align: 'nearest',
   *   margin: 0.1
   * });
   *
   * @example
   * // Zoom to 100ns viewport centered on timestamp
   * timeline.panIntoView(Time.fromRaw(1500n), {
   *   align: 'zoom',
   *   zoomWidth: 100
   * });
   */
  panIntoView(ts: time, options?: PanInstantIntoViewOptions): void;

  /**
   * Pan (and optionally zoom) the viewport to make a time span visible.
   *
   * Depending on alignment mode:
   * - 'center': Centers the span's midpoint in the viewport
   * - 'nearest': Pans minimally to bring entire span into view (default)
   * - 'zoom': Adjusts viewport to exactly fit the span (with optional margin)
   *
   * For 'center' and 'nearest' modes, the viewport duration is preserved and the
   * result is clamped to trace boundaries. For 'zoom' mode, the viewport is
   * resized to fit the span.
   *
   * @param start - The start of the time span.
   * @param end - The end of the time span.
   * @param options - Optional configuration:
   *   - align: How to position the span ('center' | 'nearest' | 'zoom')
   *   - margin: Safe zone as fraction of viewport (e.g., 0.1 = 10% margin)
   *
   * @example
   * // Center span [1200, 1400) in viewport
   * timeline.panSpanIntoView(Time.fromRaw(1200n), Time.fromRaw(1400n), {
   *   align: 'center'
   * });
   *
   * @example
   * // Bring span into view with minimal panning
   * timeline.panSpanIntoView(Time.fromRaw(1200n), Time.fromRaw(1400n), {
   *   align: 'nearest',
   *   margin: 0.05
   * });
   *
   * @example
   * // Zoom to exactly fit span with 10% margin on each side
   * timeline.panSpanIntoView(Time.fromRaw(1200n), Time.fromRaw(1400n), {
   *   align: 'zoom',
   *   margin: 0.1
   * });
   */
  panSpanIntoView(start: time, end: time, options?: PanIntoViewOptions): void;

  /**
   * Directly set the visible viewport to a specific time span.
   *
   * The provided span is automatically clamped to:
   * - Minimum duration (to prevent excessive zoom)
   * - Trace boundaries (to keep viewport within valid trace time range)
   *
   * Use this for precise control over the viewport. For user-facing navigation,
   * prefer pan(), zoom(), panIntoView(), or panSpanIntoView().
   *
   * @param span - The new viewport as a high-precision time span.
   *
   * @example
   * // Set viewport to [1000, 1500)
   * timeline.setVisibleWindow(
   *   HighPrecisionTimeSpan.fromTime(
   *     Time.fromRaw(1000n),
   *     Time.fromRaw(1500n)
   *   )
   * );
   */
  setVisibleWindow(span: HighPrecisionTimeSpan): void;

  // A span representing the current viewport location.
  readonly visibleWindow: HighPrecisionTimeSpan;

  // Render a vertical line on the timeline at this timestamp.
  hoverCursorTimestamp: time | undefined;

  hoveredNoteTimestamp: time | undefined;
  highlightedSliceId: number | undefined;

  hoveredUtid: number | undefined;
  hoveredPid: bigint | undefined;

  // This value defines the time of the origin of the time axis in trace time.
  // Depending on the timestamp format setting, this value can change:
  // E.g.
  // - Raw - origin = 0
  // - Seconds - origin = trace.start.
  // - Realtime - origin = midnight before trace.start.
  getTimeAxisOrigin(): time;

  // Get a time in the current domain as specified by timestampOffset.
  toDomainTime(ts: time): time;

  // These control how timestamps and durations are formatted throughout the UI
  timestampFormat: TimestampFormat;
  durationPrecision: DurationPrecision;
  customTimezoneOffset: number;
  timezoneOverride: Setting<string>;
}
