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
  // range and control zoom behavior via the [behavior] parameter:
  //  - 'pan' (default): Just pan to center the range without changing zoom.
  //  - 'focus': Smart zoom that centers and zooms to fit the selection:
  //    - For instant events (duration = 0), zoom in by 99.8%.
  //    - For events with duration, make them fill 80% of the viewport.
  //  - {viewPercentage: number}: Explicitly zoom so the range fills this
  //    percentage of the viewport (0.0 < viewPercentage <= 1.0).
  time?: {
    start: time;
    end?: time;
    behavior?: 'pan' | 'focus' | {viewPercentage: number};
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
