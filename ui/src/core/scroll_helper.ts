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
import {ScrollToArgs} from '../public/scroll_helper';
import {WorkspaceManager} from '../public/workspace';
import {raf} from './raf_scheduler';
import {TimelineImpl} from './timeline';
import {TrackManagerImpl} from './track_manager';

// A helper class to help jumping to tracks and time ranges.
// This class must NOT alter in any way the selection status. That
// responsibility belongs to SelectionManager (which uses this).
export class ScrollHelper {
  constructor(
    private timeline: TimelineImpl,
    private workspace: WorkspaceManager,
    private trackManager: TrackManagerImpl,
  ) {}

  // See comments in ScrollToArgs for the intended semantics.
  scrollTo(args: ScrollToArgs) {
    const {time, track} = args;
    raf.scheduleCanvasRedraw();

    if (time !== undefined) {
      const end = time.end ?? time.start;
      const behavior = time.behavior ?? 'pan'; // Default to pan

      if (typeof behavior === 'object' && 'viewPercentage' in behavior) {
        // Explicit zoom percentage
        this.focusHorizontalRangePercentage(
          time.start,
          end,
          behavior.viewPercentage,
        );
      } else if (behavior === 'focus') {
        // Smart focus: zoom and pan to center the event
        this.focusHorizontalRange(time.start, end);
      } else {
        // Pan: just move the viewport without changing zoom
        this.timeline.panSpanIntoView(time.start, end, {
          align: 'nearest',
          margin: 0.1,
        });
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

    // For instant events (duration = 0), just pan to center without zoom
    if (aoi.duration === 0) {
      this.timeline.panSpanIntoView(start, end, {align: 'center'});
      return;
    }

    if (viewPercentage <= 0.0 || viewPercentage > 1.0) {
      console.warn(
        'Invalid value for [viewPercentage]. ' +
          'Value must be between 0.0 (exclusive) and 1.0 (inclusive).',
      );
      // Default to 50%.
      viewPercentage = 0.5;
    }

    this.timeline.panSpanIntoView(start, end, {
      align: 'zoom',
      margin: (1.0 - viewPercentage) / 2,
    });
  }

  private focusHorizontalRange(start: time, end: time): void {
    // Handle instant events (duration = 0) specially
    if (start === end) {
      // For instant events, zoom in by 99.8% (new duration = 0.2% of current)
      // This value (0.002) was chosen based on heuristic testing.
      // TODO(lalitm): This should ideally use the actual viewport width in
      // pixels to calculate a precise zoom level (e.g., make 1px at current
      // scale fill 80% of viewport), but plumbing viewport width through to
      // ScrollHelper is architecturally difficult right now.
      this.timeline.panIntoView(start, {
        align: 'zoom',
        zoomWidth: 0.002 * this.timeline.visibleWindow.duration,
      });
    } else {
      // 10% padding on each side means the range fills 80% of the viewport
      this.timeline.panSpanIntoView(start, end, {align: 'zoom', margin: 0.1});
    }
  }

  private verticalScrollToTrack(trackUri: string, openGroup: boolean) {
    // Find the actual track node that uses that URI, we need various properties
    // from it.
    const trackNode = this.workspace.currentWorkspace.getTrackByUri(trackUri);
    if (!trackNode) return;

    // Try finding the track directly.
    const element = document.getElementById(trackNode.id);
    if (element) {
      // block: 'nearest' means that it will only scroll if the track is not
      // currently in view.
      element.scrollIntoView({behavior: 'smooth', block: 'nearest'});
      return;
    }

    // If we get here, the element for this track was not present in the DOM,
    // which might be because it's inside a collapsed group.
    if (openGroup) {
      // Try to reveal the track node in the workspace by opening up all
      // ancestor groups, and mark the track URI to be scrolled to in the
      // future.
      trackNode.reveal();
      this.trackManager.scrollToTrackNodeId = trackNode.id;
    } else {
      // Find the closest visible ancestor of our target track and scroll to
      // that instead.
      const container = trackNode.findClosestVisibleAncestor();
      document
        .getElementById(container.id)
        ?.scrollIntoView({behavior: 'smooth', block: 'nearest'});
    }
  }
}
