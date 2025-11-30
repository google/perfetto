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
}

export interface PanInstantIntoViewOptions extends PanIntoViewOptions {
  // When zooming into an instant which has no inherent width, this value
  // defines what the new viewport width should be. If omitted, zoom is the same
  // as center for instants.
  readonly zoomWidth?: number;
}

export interface Timeline {
  pan(delta: number): void;
  zoom(factor: number, centerPoint?: number): void;
  panIntoView(ts: time, options?: PanInstantIntoViewOptions): void;
  panSpanIntoView(start: time, end: time, options?: PanIntoViewOptions): void;
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
