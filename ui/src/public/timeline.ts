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

/**
 * Defines the various formats for displaying timestamps in the UI.
 */
export enum TimestampFormat {
  /** Displays time as a timecode (e.g., HH:MM:SS.mmm). */
  Timecode = 'timecode',
  /** Displays raw trace nanoseconds. */
  TraceNs = 'traceNs',
  /** Displays raw trace nanoseconds, formatted according to locale. */
  TraceNsLocale = 'traceNsLocale',
  /** Displays time in seconds. */
  Seconds = 'seconds',
  /** Displays time in milliseconds. */
  Milliseconds = 'milliseconds',
  /** Displays time in microseconds. */
  Microseconds = 'microseconds',
  /** Displays time in UTC format. */
  UTC = 'utc',
  /** Displays time in a custom timezone. */
  CustomTimezone = 'customTimezone',
  /** Displays time in the trace's timezone. */
  TraceTz = 'traceTz',
}

/**
 * Defines the precision for displaying durations in the UI.
 */
export enum DurationPrecision {
  /** Displays full precision for durations. */
  Full = 'full',
  /** Displays human-readable durations (e.g., 1h 2m 3s). */
  HumanReadable = 'human_readable',
}

/**
 * Manages the interactive timeline and viewport.
 *
 * The timeline allows users to navigate through the trace, zoom in and out,
 * and interact with various time-based elements.
 */
export interface Timeline {
  /**
   * Brings a specific timestamp into the current viewport.
   * @param ts The timestamp to pan to.
   */
  panToTimestamp(ts: time): void;

  /**
   * Sets the start and end times of the current viewport.
   * @param start The start timestamp of the viewport.
   * @param end The end timestamp of the viewport.
   */
  setViewportTime(start: time, end: time): void;

  /**
   * A span representing the current visible time range in the viewport.
   */
  readonly visibleWindow: HighPrecisionTimeSpan;

  /**
   * The timestamp where the hover cursor is currently located on the timeline.
   * Setting this value will render a vertical line at the specified timestamp.
   */
  hoverCursorTimestamp: time | undefined;

  /**
   * The timestamp of a hovered note on the timeline.
   * Setting this value will highlight the corresponding note.
   */
  hoveredNoteTimestamp: time | undefined;

  /**
   * The ID of the currently highlighted slice.
   * Setting this value will highlight the corresponding slice in the UI.
   */
  highlightedSliceId: number | undefined;

  /**
   * The UTID (Unique Thread ID) of the currently hovered thread.
   */
  hoveredUtid: number | undefined;

  /**
   * The PID (Process ID) of the currently hovered process.
   */
  hoveredPid: bigint | undefined;

  /**
   * Gets the time of the origin of the time axis in trace time.
   *
   * Depending on the timestamp format setting, this value can change:
   * - Raw: origin = 0
   * - Seconds: origin = trace.start
   * - Realtime: origin = midnight before trace.start
   *
   * @returns The origin timestamp of the time axis.
   */
  getTimeAxisOrigin(): time;

  /**
   * Converts a timestamp to a time in the current domain, as specified by
   * the timestamp offset.
   * @param ts The timestamp to convert.
   * @returns The timestamp in the current domain.
   */
  toDomainTime(ts: time): time;

  /**
   * Controls how timestamps are formatted throughout the UI.
   */
  timestampFormat: TimestampFormat;

  /**
   * Controls how durations are formatted throughout the UI.
   */
  durationPrecision: DurationPrecision;

  /**
   * The custom timezone offset in minutes.
   */
  customTimezoneOffset: number;

  /**
   * The setting for overriding the default timezone.
   */
  timezoneOverride: Setting<string>;
}
