// Copyright (C) 2019 The Android Open Source Project
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

import {time} from '../base/time';
import {Actions} from '../common/actions';
import {
  HighPrecisionTime,
  HighPrecisionTimeSpan,
} from '../common/high_precision_time';
import {getContainingTrackId} from '../common/state';

import {globals} from './globals';


// Given a timestamp, if |ts| is not currently in view move the view to
// center |ts|, keeping the same zoom level.
export function horizontalScrollToTs(ts: time) {
  const time = HighPrecisionTime.fromTime(ts);
  const visibleWindow = globals.frontendLocalState.visibleWindowTime;
  if (!visibleWindow.contains(time)) {
    // TODO(hjd): This is an ugly jump, we should do a smooth pan instead.
    const halfDuration = visibleWindow.duration.divide(2);
    const newStart = time.sub(halfDuration);
    const newWindow = new HighPrecisionTimeSpan(
        newStart, newStart.add(visibleWindow.duration));
    globals.frontendLocalState.updateVisibleTime(newWindow);
  }
}

// Given a start and end timestamp (in ns), move the viewport to center this
// range and zoom if necessary:
// - If [viewPercentage] is specified, the viewport will be zoomed so that
//   the given time range takes up this percentage of the viewport.
// The following scenarios assume [viewPercentage] is undefined.
// - If the new range is more than 50% of the viewport, zoom out to a level
// where
//   the range is 1/5 of the viewport.
// - If the new range is already centered, update the zoom level for the
// viewport
//   to cover 1/5 of the viewport.
// - Otherwise, preserve the zoom range.
export function focusHorizontalRange(
    start: time, end: time, viewPercentage?: number) {
  const visible = globals.frontendLocalState.visibleWindowTime;
  const trace = globals.stateTraceTime();
  const select = HighPrecisionTimeSpan.fromTime(start, end);

  if (viewPercentage !== undefined) {
    if (viewPercentage <= 0.0 || viewPercentage > 1.0) {
      console.warn(
          'Invalid value for [viewPercentage]. ' +
              'Value must be between 0.0 (exclusive) and 1.0 (inclusive).',
      );
      // Default to 50%.
      viewPercentage = 0.5;
    }
    const paddingPercentage = 1.0 - viewPercentage;
    const paddingTime = select.duration.multiply(paddingPercentage);
    const halfPaddingTime = paddingTime.divide(2);
    globals.frontendLocalState.updateVisibleTime(select.pad(halfPaddingTime));
    return;
  }
  // If the range is too large to fit on the current zoom level, resize.
  if (select.duration.gt(visible.duration.multiply(0.5))) {
    const paddedRange = select.pad(select.duration.multiply(2));
    globals.frontendLocalState.updateVisibleTime(paddedRange);
    return;
  }
  // Calculate the new visible window preserving the zoom level.
  let newStart = select.midpoint.sub(visible.duration.divide(2));
  let newEnd = select.midpoint.add(visible.duration.divide(2));

  // Adjust the new visible window if it intersects with the trace boundaries.
  // It's needed to make the "update the zoom level if visible window doesn't
  // change" logic reliable.
  if (newEnd.gt(trace.end)) {
    newStart = trace.end.sub(visible.duration);
    newEnd = trace.end;
  }
  if (newStart.lt(trace.start)) {
    newStart = trace.start;
    newEnd = trace.start.add(visible.duration);
  }

  const view = new HighPrecisionTimeSpan(newStart, newEnd);

  // If preserving the zoom doesn't change the visible window, update the zoom
  // level.
  if (view.start.eq(visible.start) && view.end.eq(visible.end)) {
    const padded = select.pad(select.duration.multiply(2));
    globals.frontendLocalState.updateVisibleTime(padded);
  } else {
    globals.frontendLocalState.updateVisibleTime(view);
  }
}

// Given a track id, find a track with that id and scroll it into view. If the
// track is nested inside a track group, scroll to that track group instead.
// If |openGroup| then open the track group and scroll to the track.
export function verticalScrollToTrack(
    trackKey: string|number, openGroup = false) {
  const trackKeyString = `${trackKey}`;
  const track = document.querySelector('#track_' + trackKeyString);

  if (track) {
    // block: 'nearest' means that it will only scroll if the track is not
    // currently in view.
    track.scrollIntoView({behavior: 'smooth', block: 'nearest'});
    return;
  }

  let trackGroup = null;
  const trackGroupId = getContainingTrackId(globals.state, trackKeyString);
  if (trackGroupId) {
    trackGroup = document.querySelector('#track_' + trackGroupId);
  }

  if (!trackGroupId || !trackGroup) {
    console.error(`Can't scroll, track (${trackKeyString}) not found.`);
    return;
  }

  // The requested track is inside a closed track group, either open the track
  // group and scroll to the track or just scroll to the track group.
  if (openGroup) {
    // After the track exists in the dom, it will be scrolled to.
    globals.frontendLocalState.scrollToTrackKey = trackKey;
    globals.dispatch(Actions.toggleTrackGroupCollapsed({trackGroupId}));
    return;
  } else {
    trackGroup.scrollIntoView({behavior: 'smooth', block: 'nearest'});
  }
}


// Scroll vertically and horizontally to reach track (|trackKey|) at |ts|.
export function scrollToTrackAndTs(
    trackKey: string|number|undefined, ts: time, openGroup = false) {
  if (trackKey !== undefined) {
    verticalScrollToTrack(trackKey, openGroup);
  }
  horizontalScrollToTs(ts);
}

// Scroll vertically and horizontally to a track and time range
export function reveal(
    trackKey: string|number, start: time, end: time, openGroup = false) {
  verticalScrollToTrack(trackKey, openGroup);
  focusHorizontalRange(start, end);
}
