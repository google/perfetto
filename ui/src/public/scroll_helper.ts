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

import {time} from '../base/time';

/**
 * A helper to scroll to a combination of tracks and time ranges.
 * This exist to decouple the selection logic to the scrolling logic. Nothing in
 * this file changes the selection status. Use SelectionManager for that.
 */
export interface ScrollToArgs {
  // Given a start and end timestamp (in ns), move the viewport to center this
  //  range and zoom if necessary:
  //  - If [viewPercentage] is specified, the viewport will be zoomed so that
  //    the given time range takes up this percentage of the viewport.
  //  The following scenarios assume [viewPercentage] is undefined.
  //  - If the new range is more than 50% of the viewport, zoom out to a level
  //  where
  //    the range is 1/5 of the viewport.
  //  - If the new range is already centered, update the zoom level for the
  //  viewport
  //    to cover 1/5 of the viewport.
  //  - Otherwise, preserve the zoom range.
  //
  time?: {
    start: time;
    end?: time;
    viewPercentage?: number;
  };
  // Find the track with a given uri in the current workspace and scroll it into
  // view. Iftrack is nested inside a track group, scroll to that track group
  // instead. If `expandGroup` == true, open the track group and scroll to the
  // track.
  // TODO(primiano): 90% of the times we seem to want expandGroup: true, so we
  // should probably flip the default value, and pass false in the few places
  // where we do NOT want this behavior.
  track?: {
    uri: string;
    expandGroup?: boolean;
  };
}

// TODO(primiano): remove this injection once we plumb Trace into all the
// components. Right now too many places need this. This is a temporary solution
// to avoid too many invasive refactorings at once.

type ScrollToFunction = (a: ScrollToArgs) => void;
let _scrollToFunction: ScrollToFunction | undefined = undefined;

// If a Trace object is avilable, prefer Trace.scrollTo(). It points to the
// same function.
export function scrollTo(args: ScrollToArgs) {
  _scrollToFunction?.(args);
}

export function setScrollToFunction(f: ScrollToFunction | undefined) {
  _scrollToFunction = f;
}
