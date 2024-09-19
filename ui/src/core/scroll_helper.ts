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
import {escapeCSSSelector} from '../base/utils';
import {ScrollToArgs} from 'src/public/scroll_helper';
import {TraceInfo} from '../public/trace_info';
import {Workspace} from '../public/workspace';
import {raf} from './raf_scheduler';
import {TimelineImpl} from './timeline';
import {TrackManagerImpl} from './track_manager';

// A helper class to help jumping to tracks and time ranges.
// This class must NOT alter in any way the selection status. That
// responsibility belongs to SelectionManager (which uses this).
export class ScrollHelper {
  constructor(
    private traceInfo: TraceInfo,
    private timeline: TimelineImpl,
    private workspace: Workspace,
    private trackManager: TrackManagerImpl,
  ) {}

  // See comments in ScrollToArgs for the intended semantics.
  scrollTo(args: ScrollToArgs) {
    const {time, track} = args;
    raf.scheduleRedraw();

    if (time !== undefined) {
      if (time.end === undefined) {
        this.timeline.panToTimestamp(time.start);
      } else if (time.viewPercentage !== undefined) {
        this.focusHorizontalRangePercentage(
          time.start,
          time.end,
          time.viewPercentage,
        );
      } else {
        this.focusHorizontalRange(time.start, time.end);
      }
    }

    if (track !== undefined) {
      this.verticalScrollToTrack(track.uri, track.expandGroup ?? false);
    }
  }

  private focusHorizontalRangePercentage(
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
    this.timeline.updateVisibleTimeHP(aoi.pad(halfPaddingTime));
  }

  private focusHorizontalRange(start: time, end: time): void {
    const visible = this.timeline.visibleWindow;
    const aoi = HighPrecisionTimeSpan.fromTime(start, end);
    const fillRatio = 5; // Default amount to make the AOI fill the viewport
    const padRatio = (fillRatio - 1) / 2;

    // If the area of interest already fills more than half the viewport, zoom
    // out so that the AOI fills 20% of the viewport
    if (aoi.duration * 2 > visible.duration) {
      const padded = aoi.pad(aoi.duration * padRatio);
      this.timeline.updateVisibleTimeHP(padded);
    } else {
      // Center visible window on the middle of the AOI, preserving zoom level.
      const newStart = aoi.midpoint.subNumber(visible.duration / 2);

      // Adjust the new visible window if it intersects with the trace boundaries.
      // It's needed to make the "update the zoom level if visible window doesn't
      // change" logic reliable.
      const newVisibleWindow = new HighPrecisionTimeSpan(
        newStart,
        visible.duration,
      ).fitWithin(this.traceInfo.start, this.traceInfo.end);

      // If preserving the zoom doesn't change the visible window, consider this
      // to be the "second" hotkey press, so just make the AOI fill 20% of the
      // viewport
      if (newVisibleWindow.equals(visible)) {
        const padded = aoi.pad(aoi.duration * padRatio);
        this.timeline.updateVisibleTimeHP(padded);
      } else {
        this.timeline.updateVisibleTimeHP(newVisibleWindow);
      }
    }
  }

  private verticalScrollToTrack(trackUri: string, openGroup: boolean) {
    const dom = document.querySelector('#track_' + escapeCSSSelector(trackUri));
    if (dom) {
      // block: 'nearest' means that it will only scroll if the track is not
      // currently in view.
      dom.scrollIntoView({behavior: 'smooth', block: 'nearest'});
      return;
    }

    // If we get here, the element for this track was not present in the DOM,
    // this might be because it's inside a collapsed group.
    // Find the track node in the current workspace, and reveal it.
    const trackNode = this.workspace.getTrackByUri(trackUri);
    if (!trackNode) return;

    if (openGroup) {
      trackNode.reveal();
      this.trackManager.scrollToTrackUriOnCreate = trackUri;
      return;
    }

    // Find the first closed ancestor of our target track.
    const groupNode = trackNode.closestVisibleAncestor;
    if (groupNode) {
      document
        .querySelector('#track_' + groupNode.uri)
        ?.scrollIntoView({behavior: 'smooth', block: 'nearest'});
    }
  }
}
