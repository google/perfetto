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
import {escapeCSSSelector, exists} from '../base/utils';
import {Actions} from '../common/actions';
import {HighPrecisionTime} from '../common/high_precision_time';
import {HighPrecisionTimeSpan} from '../common/high_precision_time_span';
import {getContainingGroupKey} from '../common/state';
import {raf} from '../core/raf_scheduler';
import {globals} from './globals';

// Given a timestamp, if |ts| is not currently in view move the view to
// center |ts|, keeping the same zoom level.
export function horizontalScrollToTs(ts: time) {
  const visibleWindow = globals.timeline.visibleWindow;
  if (!visibleWindow.contains(ts)) {
    // TODO(hjd): This is an ugly jump, we should do a smooth pan instead.
    const halfDuration = visibleWindow.duration / 2;
    const newStart = new HighPrecisionTime(ts).subNumber(halfDuration);
    const newWindow = new HighPrecisionTimeSpan(
      newStart,
      visibleWindow.duration,
    );
    globals.timeline.updateVisibleTimeHP(newWindow);
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
  start: time,
  end: time,
  viewPercentage?: number,
): void {
  if (exists(viewPercentage)) {
    focusHorizontalRangePercentage(start, end, viewPercentage);
  } else {
    focusHorizontalRangeImpl(start, end);
  }
}

// Given a track id, find a track with that id and scroll it into view. If the
// track is nested inside a track group, scroll to that track group instead.
// If |openGroup| then open the track group and scroll to the track.
export function verticalScrollToTrack(trackKey: string, openGroup = false) {
  const track = document.querySelector('#track_' + escapeCSSSelector(trackKey));

  if (track) {
    // block: 'nearest' means that it will only scroll if the track is not
    // currently in view.
    track.scrollIntoView({behavior: 'smooth', block: 'nearest'});
    return;
  }

  let trackGroup = null;
  const groupKey = getContainingGroupKey(globals.state, trackKey);
  if (groupKey) {
    trackGroup = document.querySelector('#track_' + groupKey);
  }

  if (!groupKey || !trackGroup) {
    console.error(`Can't scroll, track (${trackKey}) not found.`);
    return;
  }

  // The requested track is inside a closed track group, either open the track
  // group and scroll to the track or just scroll to the track group.
  if (openGroup) {
    // After the track exists in the dom, it will be scrolled to.
    globals.scrollToTrackKey = trackKey;
    globals.dispatch(Actions.toggleTrackGroupCollapsed({groupKey}));
    return;
  } else {
    trackGroup.scrollIntoView({behavior: 'smooth', block: 'nearest'});
  }
}

// Scroll vertically and horizontally to reach track (|trackKey|) at |ts|.
export function scrollToTrackAndTs(
  trackKey: string | undefined,
  ts: time,
  openGroup = false,
) {
  if (trackKey !== undefined) {
    verticalScrollToTrack(trackKey, openGroup);
  }
  horizontalScrollToTs(ts);
}

// Scroll vertically and horizontally to a track and time range
export function reveal(
  trackKey: string,
  start: time,
  end: time,
  openGroup = false,
) {
  verticalScrollToTrack(trackKey, openGroup);
  focusHorizontalRange(start, end);
}

function focusHorizontalRangePercentage(
  start: time,
  end: time,
  viewPercentage: number,
): void {
  const aoi = HighPrecisionTimeSpan.fromTime(start, end);

  if (viewPercentage <= 0.0 || viewPercentage > 1.0) {
    console.warn(
      'Invalid value for [viewPercentage]. ' +
        'Value must be between 0.0 (exclusive) and 1.0 (inclusive).',
    );
    // Default to 50%.
    viewPercentage = 0.5;
  }
  const paddingPercentage = 1.0 - viewPercentage;
  const halfPaddingTime = (aoi.duration * paddingPercentage) / 2;
  globals.timeline.updateVisibleTimeHP(aoi.pad(halfPaddingTime));

  raf.scheduleRedraw();
}

function focusHorizontalRangeImpl(start: time, end: time): void {
  const visible = globals.timeline.visibleWindow;
  const aoi = HighPrecisionTimeSpan.fromTime(start, end);
  const fillRatio = 5; // Default amount to make the AOI fill the viewport
  const padRatio = (fillRatio - 1) / 2;

  // If the area of interest already fills more than half the viewport, zoom out
  // so that the AOI fills 20% of the viewport
  if (aoi.duration * 2 > visible.duration) {
    const padded = aoi.pad(aoi.duration * padRatio);
    globals.timeline.updateVisibleTimeHP(padded);
  } else {
    // Center visible window on the middle of the AOI, preserving the zoom level
    const newStart = aoi.midpoint.subNumber(visible.duration / 2);

    // Adjust the new visible window if it intersects with the trace boundaries.
    // It's needed to make the "update the zoom level if visible window doesn't
    // change" logic reliable.
    const newVisibleWindow = new HighPrecisionTimeSpan(
      newStart,
      visible.duration,
    ).fitWithin(globals.traceContext.start, globals.traceContext.end);

    // If preserving the zoom doesn't change the visible window, consider this
    // to be the "second" hotkey press, so just make the AOI fill 20% of the
    // viewport
    if (newVisibleWindow.equals(visible)) {
      const padded = aoi.pad(aoi.duration * padRatio);
      globals.timeline.updateVisibleTimeHP(padded);
    } else {
      globals.timeline.updateVisibleTimeHP(newVisibleWindow);
    }
  }

  raf.scheduleRedraw();
}
