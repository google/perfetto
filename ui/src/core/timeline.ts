// Copyright (C) 2018 The Android Open Source Project
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

import {assertUnreachable} from '../base/logging';
import {Time, time, TimeSpan, timezoneOffsetMap} from '../base/time';
import {HighPrecisionTimeSpan} from '../base/high_precision_time_span';
import {raf} from './raf_scheduler';
import {HighPrecisionTime} from '../base/high_precision_time';
import {DurationPrecision, Timeline, TimestampFormat} from '../public/timeline';
import {TraceInfo} from '../public/trace_info';
import {Setting} from '../public/settings';

const MIN_DURATION = 10;

/**
 * State that is shared between several frontend components, but not the
 * controller. This state is updated at 60fps.
 */
export class TimelineImpl implements Timeline {
  private _visibleWindow: HighPrecisionTimeSpan;
  private _hoverCursorTimestamp?: time;
  private _highlightedSliceId?: number;
  private _hoveredNoteTimestamp?: time;

  // TODO(stevegolton): These are currently only referenced by the cpu slice
  // tracks and the process summary tracks. We should just make this a local
  // property of the cpu slice tracks and ignore them in the process tracks.
  private _hoveredUtid?: number;
  private _hoveredPid?: bigint;

  // This is used to mark the timeline of the area that is currently being
  // selected.
  //
  // TODO(stevegolton): This shouldn't really be in the global timeline state,
  // it's really only a concept of the viewer page and should be moved there
  // instead.
  selectedSpan?: {start: time; end: time};

  get highlightedSliceId() {
    return this._highlightedSliceId;
  }

  set highlightedSliceId(x) {
    this._highlightedSliceId = x;
    raf.scheduleCanvasRedraw();
  }

  get hoveredNoteTimestamp() {
    return this._hoveredNoteTimestamp;
  }

  set hoveredNoteTimestamp(x) {
    this._hoveredNoteTimestamp = x;
    raf.scheduleCanvasRedraw();
  }

  get hoveredUtid() {
    return this._hoveredUtid;
  }

  set hoveredUtid(x) {
    this._hoveredUtid = x;
    raf.scheduleCanvasRedraw();
  }

  get hoveredPid() {
    return this._hoveredPid;
  }

  set hoveredPid(x) {
    this._hoveredPid = x;
    raf.scheduleCanvasRedraw();
  }

  constructor(
    private readonly traceInfo: TraceInfo,
    private readonly _timestampFormat: Setting<TimestampFormat>,
    private readonly _durationPrecision: Setting<DurationPrecision>,
    readonly timezoneOverride: Setting<string>,
  ) {
    this._visibleWindow = HighPrecisionTimeSpan.fromTime(
      traceInfo.start,
      traceInfo.end,
    );
  }

  // TODO: there is some redundancy in the fact that both |visibleWindowTime|
  // and a |timeScale| have a notion of time range. That should live in one
  // place only.

  zoomVisibleWindow(ratio: number, centerPoint: number) {
    this._visibleWindow = this._visibleWindow
      .scale(ratio, centerPoint, MIN_DURATION)
      .fitWithin(this.traceInfo.start, this.traceInfo.end);

    raf.scheduleCanvasRedraw();
  }

  panVisibleWindow(delta: number) {
    this._visibleWindow = this._visibleWindow
      .translate(delta)
      .fitWithin(this.traceInfo.start, this.traceInfo.end);

    raf.scheduleCanvasRedraw();
  }

  // Given a timestamp, if |ts| is not currently in view move the view to
  // center |ts|, keeping the same zoom level.
  panToTimestamp(ts: time) {
    if (this._visibleWindow.contains(ts)) return;
    // TODO(hjd): This is an ugly jump, we should do a smooth pan instead.
    const halfDuration = this.visibleWindow.duration / 2;
    const newStart = new HighPrecisionTime(ts).subNumber(halfDuration);
    const newWindow = new HighPrecisionTimeSpan(
      newStart,
      this._visibleWindow.duration,
    );
    this.updateVisibleTimeHP(newWindow);
  }

  // Set visible window using an integer time span
  updateVisibleTime(ts: TimeSpan) {
    this.updateVisibleTimeHP(HighPrecisionTimeSpan.fromTime(ts.start, ts.end));
  }

  // TODO(primiano): we ended up with two entry-points for the same function,
  // unify them.
  setViewportTime(start: time, end: time): void {
    this.updateVisibleTime(new TimeSpan(start, end));
  }

  moveStart(delta: number) {
    this.updateVisibleTimeHP(
      new HighPrecisionTimeSpan(
        this._visibleWindow.start.addNumber(delta),
        this.visibleWindow.duration - delta,
      ),
    );
  }

  moveEnd(delta: number) {
    this.updateVisibleTimeHP(
      new HighPrecisionTimeSpan(
        this._visibleWindow.start,
        this.visibleWindow.duration + delta,
      ),
    );
  }

  // Set visible window using a high precision time span
  updateVisibleTimeHP(ts: HighPrecisionTimeSpan) {
    this._visibleWindow = ts
      .clampDuration(MIN_DURATION)
      .fitWithin(this.traceInfo.start, this.traceInfo.end);

    raf.scheduleCanvasRedraw();
  }

  // Get the bounds of the visible window as a high-precision time span
  get visibleWindow(): HighPrecisionTimeSpan {
    return this._visibleWindow;
  }

  get hoverCursorTimestamp(): time | undefined {
    return this._hoverCursorTimestamp;
  }

  set hoverCursorTimestamp(t: time | undefined) {
    this._hoverCursorTimestamp = t;
    raf.scheduleCanvasRedraw();
  }

  /**
   * The trace time value where the timeline is considered to actually start.
   * E.g.
   *  - Raw: offset = 0
   *  - Trace: offset = trace.start
   *  - Realtime: offset = previous midnight before trace.start
   */
  getTimeAxisOrigin(): time {
    const fmt = this.timestampFormat;
    switch (fmt) {
      case TimestampFormat.Timecode:
      case TimestampFormat.Seconds:
      case TimestampFormat.Milliseconds:
      case TimestampFormat.Microseconds:
        return this.traceInfo.start;
      case TimestampFormat.TraceNs:
      case TimestampFormat.TraceNsLocale:
        return Time.ZERO;
      case TimestampFormat.UTC:
        return getTraceMidnightInTimezone(
          this.traceInfo.start,
          this.traceInfo.unixOffset,
          0, // UTC
        );
      case TimestampFormat.CustomTimezone:
        return getTraceMidnightInTimezone(
          this.traceInfo.start,
          this.traceInfo.unixOffset,
          timezoneOffsetMap[this.timezoneOverride.get()],
        );
      case TimestampFormat.TraceTz:
        return getTraceMidnightInTimezone(
          this.traceInfo.start,
          this.traceInfo.unixOffset,
          this.traceInfo.tzOffMin,
        );
      default:
        assertUnreachable(fmt);
    }
  }

  // Convert absolute time to domain time.
  toDomainTime(ts: time): time {
    return Time.sub(ts, this.getTimeAxisOrigin());
  }

  get timestampFormat() {
    return this._timestampFormat.get();
  }

  set timestampFormat(format: TimestampFormat) {
    this._timestampFormat.set(format);
  }

  get durationPrecision() {
    return this._durationPrecision.get();
  }

  set durationPrecision(precision: DurationPrecision) {
    this._durationPrecision.set(precision);
  }

  get customTimezoneOffset(): number {
    return timezoneOffsetMap[this.timezoneOverride.get()];
  }
}

/**
 * Returns the timestamp of the midnight before the trace starts in trace time
 * units.
 *
 * @param traceStart - The trace-time timestamp of the start of the trace.
 * @param unixOffset - The offset between the timestamp and the unix epoch.
 * @param tzOffsetMins - The configured timezone offset in minutes.
 * @returns The trace-time timestamp at the first midnight before the trace
 * starts.
 */
function getTraceMidnightInTimezone(
  traceStart: time,
  unixOffset: time,
  tzOffsetMins: number,
) {
  const unixTime = Time.toDate(traceStart, unixOffset);

  // Remove the time component of the date, viewed in the specific
  // timezone we're looking for.
  const midnight = dateOnly(unixTime, tzOffsetMins);

  // Convert back to trace time
  return Time.fromDate(midnight, unixOffset);
}

function dateOnly(date: Date, tzOffsetMins: number) {
  // 1. Get the timestamp in milliseconds from the original date.
  const originalTimestamp = date.getTime();

  // 2. Calculate the timezone offset in milliseconds.
  const timezoneOffsetInMilliseconds = tzOffsetMins * 60 * 1000;

  // 3. Create a new Date object representing the time in the target timezone.
  //    We do this by adding our offset to the UTC time.
  const dateInTargetTimezone = new Date(
    originalTimestamp + timezoneOffsetInMilliseconds,
  );

  // 4. Now, working with this new Date object in the UTC frame,
  //    we can simply set its time components to the start of the day (midnight).
  dateInTargetTimezone.setUTCHours(0, 0, 0, 0);

  // 5. Finally, we convert this back to a timestamp and create a new Date object.
  //    This gives us the UTC timestamp of the midnight in the target timezone.
  return new Date(
    dateInTargetTimezone.getTime() - timezoneOffsetInMilliseconds,
  );
}
