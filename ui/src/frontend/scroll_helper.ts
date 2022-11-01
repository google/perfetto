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

import {Actions} from '../common/actions';
import {getContainingTrackId} from '../common/state';
import {fromNs, TimeSpan, toNs} from '../common/time';

import {globals} from './globals';

const INCOMPLETE_SLICE_TIME_S = 0.00003;

// Given a timestamp, if |ts| is not currently in view move the view to
// center |ts|, keeping the same zoom level.
export function horizontalScrollToTs(ts: number) {
  const startNs = toNs(globals.frontendLocalState.visibleWindowTime.start);
  const endNs = toNs(globals.frontendLocalState.visibleWindowTime.end);
  const currentViewNs = endNs - startNs;
  if (ts < startNs || ts > endNs) {
    // TODO(hjd): This is an ugly jump, we should do a smooth pan instead.
    globals.frontendLocalState.updateVisibleTime(new TimeSpan(
        fromNs(ts - currentViewNs / 2), fromNs(ts + currentViewNs / 2)));
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
    startTs: number, endTs: number, viewPercentage?: number) {
  const visibleDur = globals.frontendLocalState.visibleWindowTime.end -
      globals.frontendLocalState.visibleWindowTime.start;
  let selectDur = endTs - startTs;
  // TODO(altimin): We go from `ts` and `dur` to `startTs` and `endTs` and back
  // to `dur`. We should fix that.
  if (toNs(selectDur) === -1) {  // Unfinished slice
    selectDur = INCOMPLETE_SLICE_TIME_S;
    endTs = startTs;
  }

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
    const paddingTime = selectDur * paddingPercentage;
    const halfPaddingTime = paddingTime / 2;
    globals.frontendLocalState.updateVisibleTime(
        new TimeSpan(startTs - halfPaddingTime, endTs + halfPaddingTime));
    return;
  }

  // If the range is too large to fit on the current zoom level, resize.
  if (selectDur > 0.5 * visibleDur) {
    globals.frontendLocalState.updateVisibleTime(
        new TimeSpan(startTs - (selectDur * 2), endTs + (selectDur * 2)));
    return;
  }
  const midpointTs = (endTs + startTs) / 2;
  // Calculate the new visible window preserving the zoom level.
  let newStartTs = midpointTs - visibleDur / 2;
  let newEndTs = midpointTs + visibleDur / 2;

  // Adjust the new visible window if it intersects with the trace boundaries.
  // It's needed to make the "update the zoom level if visible window doesn't
  // change" logic reliable.
  if (newEndTs > globals.state.traceTime.endSec) {
    newStartTs = globals.state.traceTime.endSec - visibleDur;
    newEndTs = globals.state.traceTime.endSec;
  }
  if (newStartTs < globals.state.traceTime.startSec) {
    newStartTs = globals.state.traceTime.startSec;
    newEndTs = globals.state.traceTime.startSec + visibleDur;
  }

  const newStartNs = toNs(newStartTs);
  const newEndNs = toNs(newEndTs);

  const viewStartNs = toNs(globals.frontendLocalState.visibleWindowTime.start);
  const viewEndNs = toNs(globals.frontendLocalState.visibleWindowTime.end);

  // If preserving the zoom doesn't change the visible window, update the zoom
  // level.
  if (newStartNs === viewStartNs && newEndNs === viewEndNs) {
    globals.frontendLocalState.updateVisibleTime(
        new TimeSpan(startTs - (selectDur * 2), endTs + (selectDur * 2)));
    return;
  }
  globals.frontendLocalState.updateVisibleTime(
      new TimeSpan(newStartTs, newEndTs));
}

// Given a track id, find a track with that id and scroll it into view. If the
// track is nested inside a track group, scroll to that track group instead.
// If |openGroup| then open the track group and scroll to the track.
export function verticalScrollToTrack(
    trackId: string|number, openGroup = false) {
  const trackIdString = `${trackId}`;
  const track = document.querySelector('#track_' + trackIdString);

  if (track) {
    // block: 'nearest' means that it will only scroll if the track is not
    // currently in view.
    track.scrollIntoView({behavior: 'smooth', block: 'nearest'});
    return;
  }

  let trackGroup = null;
  const trackGroupId = getContainingTrackId(globals.state, trackIdString);
  if (trackGroupId) {
    trackGroup = document.querySelector('#track_' + trackGroupId);
  }

  if (!trackGroupId || !trackGroup) {
    console.error(`Can't scroll, track (${trackIdString}) not found.`);
    return;
  }

  // The requested track is inside a closed track group, either open the track
  // group and scroll to the track or just scroll to the track group.
  if (openGroup) {
    // After the track exists in the dom, it will be scrolled to.
    globals.frontendLocalState.scrollToTrackId = trackId;
    globals.dispatch(Actions.toggleTrackGroupCollapsed({trackGroupId}));
    return;
  } else {
    trackGroup.scrollIntoView({behavior: 'smooth', block: 'nearest'});
  }
}


// Scroll vertically and horizontally to reach track (|trackId|) at |ts|.
export function scrollToTrackAndTs(
    trackId: string|number|undefined, ts: number, openGroup = false) {
  if (trackId !== undefined) {
    verticalScrollToTrack(trackId, openGroup);
  }
  horizontalScrollToTs(ts);
}
