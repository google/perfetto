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

import {HighPrecisionTimeSpan} from '../base/high_precision_time_span';
import {HighPrecisionTime} from '../base/high_precision_time';
import {assertUnreachable} from '../base/logging';
import {Time, time, timezoneOffsetMap} from '../base/time';
import {Setting} from '../public/settings';
import {
  DurationPrecision,
  PanInstantIntoViewOptions,
  PanIntoViewOptions,
  Timeline,
  TimestampFormat,
} from '../public/timeline';
import {TraceInfo} from '../public/trace_info';
import {raf} from './raf_scheduler';

/**
 * State that is shared between several frontend components, but not the
 * controller. This state is updated at 60fps.
 */
export class TimelineImpl implements Timeline {
  readonly MIN_DURATION = 10;
  private readonly ANIMATION_DURATION_MS = 300;
  private readonly SPAM_DETECTION_THRESHOLD_MS = 300;

  private _visibleWindow: HighPrecisionTimeSpan;
  private _hoverCursorTimestamp?: time;
  private _highlightedSliceId?: number;
  private _hoveredNoteTimestamp?: time;
  private _animationStartTime?: number;
  private _animationStartWindow?: HighPrecisionTimeSpan;
  private _animationTargetWindow?: HighPrecisionTimeSpan;
  private _lastAnimationRequestTime = 0;

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

  pan(delta: number) {
    this.setVisibleWindow(
      this._visibleWindow
        .translate(delta)
        .fitWithin(this.traceInfo.start, this.traceInfo.end),
    );
  }

  zoom(ratio: number, centerPoint: number = 0.5) {
    this.setVisibleWindow(
      this._visibleWindow
        .scale(ratio, centerPoint, this.MIN_DURATION)
        .fitWithin(this.traceInfo.start, this.traceInfo.end),
    );
  }

  // Given a timestamp, if |ts| is not currently in view move the view to
  // center |ts|, keeping the same zoom level.
  panIntoView(timePoint: time, options: PanInstantIntoViewOptions = {}) {
    const {
      align = 'nearest',
      margin = 0.1,
      animation = 'ease-in-out',
      zoomWidth,
    } = options;

    const viewportDuration = this._visibleWindow.duration;
    const marginNanos = viewportDuration * margin;

    // Check if timestamp is already in view with margin
    const viewWithMargin = this._visibleWindow.pad(-marginNanos);
    if (align === 'nearest' && viewWithMargin.contains(timePoint)) {
      // Already visible with margin, no need to pan
      return;
    }

    let newViewport: HighPrecisionTimeSpan;

    switch (align) {
      case 'center':
        newViewport = new HighPrecisionTimeSpan(
          new HighPrecisionTime(timePoint).subNumber(viewportDuration / 2),
          viewportDuration,
        );
        break;
      case 'nearest':
        // Pan the minimum amount to bring timestamp into view
        if (timePoint < this._visibleWindow.start.integral) {
          // Timestamp is before view, align to left
          newViewport = new HighPrecisionTimeSpan(
            new HighPrecisionTime(timePoint).subNumber(marginNanos),
            viewportDuration,
          );
        } else {
          // Timestamp is after view, align to right
          newViewport = new HighPrecisionTimeSpan(
            new HighPrecisionTime(timePoint).subNumber(
              viewportDuration - marginNanos,
            ),
            viewportDuration,
          );
        }
        break;
      case 'zoom':
        const newDuration =
          zoomWidth !== undefined
            ? Math.max(this.MIN_DURATION, zoomWidth)
            : viewportDuration;
        newViewport = new HighPrecisionTimeSpan(
          new HighPrecisionTime(timePoint).subNumber(newDuration / 2),
          newDuration,
        );
        break;
      default:
        assertUnreachable(align);
    }

    switch (animation) {
      case 'ease-in-out':
        this.animateToWindow(newViewport);
        break;
      case 'step':
        this.setVisibleWindow(newViewport);
        break;
      default:
        assertUnreachable(animation);
    }
  }

  panSpanIntoView(start: time, end: time, options: PanIntoViewOptions = {}) {
    const {
      align = 'nearest',
      margin = 0.1,
      animation = 'ease-in-out',
    } = options;

    const duration = this._visibleWindow.duration;
    const marginNanos = duration * margin;

    const spanDuration = Number(end - start);
    const spanMidpoint = new HighPrecisionTime(start).addNumber(
      spanDuration / 2,
    );
    let newViewport: HighPrecisionTimeSpan;

    switch (align) {
      case 'center':
        // Center the midpoint of the span
        newViewport = new HighPrecisionTimeSpan(
          spanMidpoint.subNumber(duration / 2),
          duration,
        );
        break;
      case 'nearest':
        newViewport = new HighPrecisionTimeSpan(
          this.panSpanIntoViewNearest(start, end, marginNanos),
          duration,
        );
        break;
      case 'zoom':
        // Make it so that the span fits exactly within the viewport with margin
        // That is, if the margin is 10% of the viewport, the span should take up
        // 80% of the viewport.
        const newDuration = Math.max(
          this.MIN_DURATION,
          spanDuration * (1 / (1 - margin * 2)),
        );
        newViewport = new HighPrecisionTimeSpan(
          spanMidpoint.subNumber(newDuration / 2),
          newDuration,
        );
    }

    switch (animation) {
      case 'ease-in-out':
        this.animateToWindow(newViewport);
        break;
      case 'step':
        this.setVisibleWindow(newViewport);
        break;
      default:
        assertUnreachable(animation);
    }
  }

  private panSpanIntoViewNearest(start: time, end: time, marginNanos: number) {
    const viewWithMargin = this._visibleWindow.pad(-marginNanos);
    const duration = this._visibleWindow.duration;
    const spanDuration = Number(end - start);

    // Check if span is already visible with margin
    if (viewWithMargin.containsSpan(start, end)) {
      // Span fits in safe zone and is already there
      return this._visibleWindow.start;
    }

    // Check if viewport is contained within span
    if (viewWithMargin.containedBy(start, end)) {
      // Span is larger than the safe zone so there's nothing we can do to show
      // more of it - just return current position
      return this._visibleWindow.start;
    }

    // Now the behavior depends on the size of the span relative to the safe
    // zone.
    if (spanDuration < viewWithMargin.duration) {
      if (viewWithMargin.start.gte(start)) {
        // Span overlaps start - align start to safe left edge
        return new HighPrecisionTime(start).subNumber(marginNanos);
      } else {
        // Span overlaps end - align end of viewport to the end of the span
        return new HighPrecisionTime(end).subNumber(duration - marginNanos);
      }
    } else {
      // Span is wider than (or same width as) safe zone - work out whether to
      // align the start of the viewport with the start of the span, or the end
      // of the viewport with the end of the span.
      const distToAlignStart = Math.abs(
        viewWithMargin.start.subTime(start).toNumber(),
      );
      const distToAlignEnd = Math.abs(
        viewWithMargin.end.subTime(end).toNumber(),
      );
      if (distToAlignEnd < distToAlignStart) {
        // Align span end to safe right edge
        return new HighPrecisionTime(end).subNumber(duration - marginNanos);
      } else {
        // Align span start to safe left edge
        return new HighPrecisionTime(start).subNumber(marginNanos);
      }
    }
  }

  moveStart(delta: number) {
    this._visibleWindow = new HighPrecisionTimeSpan(
      this._visibleWindow.start.addNumber(delta),
      this._visibleWindow.duration - delta,
    );

    raf.scheduleCanvasRedraw();
  }

  moveEnd(delta: number) {
    this._visibleWindow = new HighPrecisionTimeSpan(
      this._visibleWindow.start,
      this._visibleWindow.duration + delta,
    );

    raf.scheduleCanvasRedraw();
  }

  // Set visible window using a high precision time span
  setVisibleWindow(ts: HighPrecisionTimeSpan) {
    this._visibleWindow = ts
      .clampDuration(this.MIN_DURATION)
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

  // Animate to a new visible window using ease-in-ease-out
  private animateToWindow(targetWindow: HighPrecisionTimeSpan) {
    const now = performance.now();

    // Detect spam: if an animation request comes within threshold of the last one,
    // or if an animation is already in progress, skip animation and use instant update
    const isSpamming =
      this._animationStartTime !== undefined ||
      now - this._lastAnimationRequestTime < this.SPAM_DETECTION_THRESHOLD_MS;

    this._lastAnimationRequestTime = now;

    if (isSpamming) {
      // Cancel any ongoing animation and jump directly to target
      if (this._animationStartTime !== undefined) {
        raf.stopAnimation(this.onAnimation);
        this._animationStartTime = undefined;
        this._animationStartWindow = undefined;
        this._animationTargetWindow = undefined;
      }
      // Use instant update instead of animation
      this.setVisibleWindow(targetWindow);
      return;
    }

    // Apply clamping to target window
    const clampedTarget = targetWindow
      .clampDuration(this.MIN_DURATION)
      .fitWithin(this.traceInfo.start, this.traceInfo.end);

    // Store animation state
    this._animationStartWindow = this._visibleWindow;
    this._animationTargetWindow = clampedTarget;
    this._animationStartTime = now;

    // Start the animation
    raf.startAnimation(this.onAnimation);
  }

  // Animation callback using ease-in-ease-out
  private onAnimation = (currentTimeMs: number) => {
    if (
      this._animationStartTime === undefined ||
      this._animationStartWindow === undefined ||
      this._animationTargetWindow === undefined
    ) {
      return;
    }

    const elapsed = currentTimeMs - this._animationStartTime;
    const progress = Math.min(elapsed / this.ANIMATION_DURATION_MS, 1);

    // Ease-in-ease-out function: 3t^2 - 2t^3
    const eased = progress * progress * (3 - 2 * progress);

    // Interpolate start position
    const startDelta =
      this._animationTargetWindow.start.toNumber() -
      this._animationStartWindow.start.toNumber();
    const newStart = this._animationStartWindow.start.addNumber(
      startDelta * eased,
    );

    // Interpolate duration
    const durationDelta =
      this._animationTargetWindow.duration -
      this._animationStartWindow.duration;
    const newDuration =
      this._animationStartWindow.duration + durationDelta * eased;

    this._visibleWindow = new HighPrecisionTimeSpan(newStart, newDuration);
    raf.scheduleCanvasRedraw();

    if (progress >= 1) {
      // Animation complete - clean up state
      this._animationStartTime = undefined;
      this._animationStartWindow = undefined;
      this._animationTargetWindow = undefined;
      raf.stopAnimation(this.onAnimation);
    }
  };
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
