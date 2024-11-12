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

import {assertTrue} from '../base/logging';
import {Time, time, TimeSpan} from '../base/time';
import {HighPrecisionTimeSpan} from '../base/high_precision_time_span';
import {Area} from '../public/selection';
import {raf} from './raf_scheduler';
import {HighPrecisionTime} from '../base/high_precision_time';
import {Timeline} from '../public/timeline';
import {timestampFormat, TimestampFormat} from './timestamp_format';
import {TraceInfo} from '../public/trace_info';

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
  private _hoveredPid?: number;

  get highlightedSliceId() {
    return this._highlightedSliceId;
  }

  set highlightedSliceId(x) {
    this._highlightedSliceId = x;
    raf.scheduleFullRedraw();
  }

  get hoveredNoteTimestamp() {
    return this._hoveredNoteTimestamp;
  }

  set hoveredNoteTimestamp(x) {
    this._hoveredNoteTimestamp = x;
    raf.scheduleFullRedraw();
  }

  get hoveredUtid() {
    return this._hoveredUtid;
  }

  set hoveredUtid(x) {
    this._hoveredUtid = x;
    raf.scheduleFullRedraw();
  }

  get hoveredPid() {
    return this._hoveredPid;
  }

  set hoveredPid(x) {
    this._hoveredPid = x;
    raf.scheduleFullRedraw();
  }

  // This is used to calculate the tracks within a Y range for area selection.
  private _selectedArea?: Area;

  constructor(private readonly traceInfo: TraceInfo) {
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

    raf.scheduleRedraw();
  }

  panVisibleWindow(delta: number) {
    this._visibleWindow = this._visibleWindow
      .translate(delta)
      .fitWithin(this.traceInfo.start, this.traceInfo.end);

    raf.scheduleRedraw();
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

  // Set the highlight box to draw
  selectArea(
    start: time,
    end: time,
    tracks = this._selectedArea ? this._selectedArea.trackUris : [],
  ) {
    assertTrue(
      end >= start,
      `Impossible select area: start [${start}] >= end [${end}]`,
    );
    this._selectedArea = {start, end, trackUris: tracks};
    raf.scheduleFullRedraw();
  }

  deselectArea() {
    this._selectedArea = undefined;
    raf.scheduleRedraw();
  }

  get selectedArea(): Area | undefined {
    return this._selectedArea;
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

  // Set visible window using a high precision time span
  updateVisibleTimeHP(ts: HighPrecisionTimeSpan) {
    this._visibleWindow = ts
      .clampDuration(MIN_DURATION)
      .fitWithin(this.traceInfo.start, this.traceInfo.end);

    raf.scheduleRedraw();
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
    raf.scheduleRedraw();
  }

  // Offset between t=0 and the configured time domain.
  timestampOffset(): time {
    const fmt = timestampFormat();
    switch (fmt) {
      case TimestampFormat.Timecode:
      case TimestampFormat.Seconds:
      case TimestampFormat.Milliseoncds:
      case TimestampFormat.Microseconds:
        return this.traceInfo.start;
      case TimestampFormat.TraceNs:
      case TimestampFormat.TraceNsLocale:
        return Time.ZERO;
      case TimestampFormat.UTC:
        return this.traceInfo.utcOffset;
      case TimestampFormat.TraceTz:
        return this.traceInfo.traceTzOffset;
      default:
        const x: never = fmt;
        throw new Error(`Unsupported format ${x}`);
    }
  }

  // Convert absolute time to domain time.
  toDomainTime(ts: time): time {
    return Time.sub(ts, this.timestampOffset());
  }
}
