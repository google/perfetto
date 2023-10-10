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
import {
  HighPrecisionTime,
  HighPrecisionTimeSpan,
} from '../common/high_precision_time';
import {getContainingTrackIds} from '../common/state';
import {TPTime} from '../common/time';

import {globals} from './globals';
import {assertFalse, assertTrue} from '../base/logging';


// Given a timestamp, if |ts| is not currently in view move the view to
// center |ts|, keeping the same zoom level.
export function horizontalScrollToTs(ts: TPTime) {
  const time = HighPrecisionTime.fromTPTime(ts);
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
    start: TPTime, end: TPTime, viewPercentage?: number) {
  console.log('focusHorizontalRange', start, end);
  const visible = globals.frontendLocalState.visibleWindowTime;
  const trace = globals.stateTraceTime();
  const select = HighPrecisionTimeSpan.fromTpTime(start, end);

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
    trackId: string|number, openGroup = false) {
  const trackIdString = `${trackId}`;
  const track = document.querySelector('#track_' + trackIdString);

  if (track) {
    // block: 'nearest' means that it will only scroll if the track is not
    // currently in view.
    track.scrollIntoView({behavior: 'smooth', block: 'nearest'});
    return;
  }

  // At this point, the requested track to reveal is not rendered because it
  // is contained by some group that is collapsed, at some level of nesting.
  // So we will either be expanding groups to reveal the track and scroll to
  // it, or we will simply reveal the closest containing group that we can.
  // That starts with finding what is the chain of containing groups' UUIDs.
  const containingIds = getContainingTrackIds(globals.state, trackIdString);
  if (!containingIds || !containingIds.length) {
    console.error(`Can't scroll, track (${trackIdString}) not found.`);
    return;
  }

  // Track groups containing the track to be revealed, in order from top
  // down, represented either as the rendered HTML element or, if not
  // rendered because an ancestor group is collapsed, its UUID
  const trackGroupsTopDown = getTrackGroupsTopDown(containingIds);

  // The requested track is inside a closed track group. Either open the track
  // group and scroll to the track or just scroll to deepest nested track group
  // that has been rendered.
  if (openGroup) {
    expandGroupsToRevealTrack(trackIdString, containingIds, trackGroupsTopDown);
  } else {
    scrollToDeepestTrackGroup(trackGroupsTopDown);
  }
}

// Get the rendered track groups in the given hierarchical chain,
// in order from topmost to deepest, as many as are rendered by
// their own container having been expanded in the UI.
// These are represented in the resulting array by |Element|.
// All further nested track groups that are not rendered are
// represented in the resulting array by their UUIDs.
// down, represented either as the rendered HTML element or, if not
// rendered because an ancestor group is collapsed, its UUID.
function getTrackGroupsTopDown(groupIds: string[]): (Element|string)[] {
  const result: (Element|string)[] = [];

  for (const trackGroupId of groupIds) {
    const group = document.querySelector('#track_' + trackGroupId);

    if (group) {
      result.push(group);
    } else {
      // Some containing group is collapsed, so this one is not rendered
      result.push(trackGroupId);
    }
  }

  return result;
}

// Expand whatever groups in the |trackGroupsTopDown| that are
// collapsed as necessary to reveal and scroll to the track
// identified by the given |trackId|. Each element of the
// |trackGroupTopDown| is identified by the corresponding UUID
// in the |containingGroupIds| array. As there is potentially
// some tail of the former that are UUIDs representing unrendered
// groups (because their container is collapsed), there may be
// overlap in these arrays.
//
// Preconditions:
//   - |trackGroupsTopDown| is non-empty
//   - |trackGroupsTopDown| and |containingGroupIds| have the same length
//   - |trackGroupsTopDown[0]| is a rendered |Element|
function expandGroupsToRevealTrack(trackId: string,
    containingGroupIds: string[],
    trackGroupsTopDown: (Element|string)[]): void {
  // After the track exists in the DOM, it will be scrolled to.
  globals.frontendLocalState.scrollToTrackId = trackId;

  assertTrue(trackGroupsTopDown.length > 0, 'No track groups to expand.');
  assertTrue(
    trackGroupsTopDown.length === containingGroupIds.length,
    'Mismatched track groups and track group IDs.',
  );
  // It should not happen that the topmost group is not rendered
  // because it has no real parent group that could be collapsed.
  assertFalse(typeof trackGroupsTopDown[0] === 'string', 'Topmost track group is unrendered.');

  trackGroupsTopDown.forEach(
      (trackGroup: Element|string, i: number) => {
    if (typeof trackGroup === 'string') {
      // Expand its parent
      if (typeof trackGroupsTopDown[i - 1] !== 'string') {
        const trackGroupId = containingGroupIds[i - 1];
        globals.dispatch(Actions.toggleTrackGroupCollapsed({trackGroupId}));
      }

      // And mark all the rest for expansion when they are created
      globals.frontendLocalState.expandTrackGroupIds.add(trackGroup);
    } else if (i === (trackGroupsTopDown.length - 1)) {
      // Need to expand the bottommost group to create the track to reveal
      const trackGroupId = containingGroupIds[i];
      globals.dispatch(Actions.toggleTrackGroupCollapsed({trackGroupId}));
    }
  });
}

// Find the deepest track group in the |trackGroupsTopDown| array
// that is a rendered |Element| and scroll to it. If none of the
// groups is rendered, then no scroll will occur.
function scrollToDeepestTrackGroup(
    trackGroupsTopDown: (Element|string)[]): void {
  for (const trackGroup of trackGroupsTopDown.slice().reverse()) {
    if (typeof trackGroup !== 'string') {
      trackGroup.scrollIntoView({behavior: 'smooth', block: 'nearest'});
      break;
    }
  }
}

// Scroll vertically and horizontally to reach track (|trackId|) at |ts|.
export function scrollToTrackAndTs(
    trackId: string|number|undefined, ts: TPTime, openGroup = false) {
  if (trackId !== undefined) {
    verticalScrollToTrack(trackId, openGroup);
  }
  horizontalScrollToTs(ts);
}

// Scroll vertically and horizontally to a track and time range
export function reveal(
    trackId: string|number, start: TPTime, end: TPTime, openGroup = false) {
  verticalScrollToTrack(trackId, openGroup);
  focusHorizontalRange(start, end);
}
