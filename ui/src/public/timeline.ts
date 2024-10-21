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

export interface Timeline {
  // Bring a timestamp into view.
  panToTimestamp(ts: time): void;

  // Move the viewport.
  setViewportTime(start: time, end: time): void;

  // A span representing the current viewport location.
  readonly visibleWindow: HighPrecisionTimeSpan;

  // Render a vertical line on the timeline at this timestamp.
  hoverCursorTimestamp: time | undefined;

  hoveredNoteTimestamp: time | undefined;
  highlightedSliceId: number | undefined;

  hoveredUtid: number | undefined;
  hoveredPid: number | undefined;

  // Get the current timestamp offset.
  timestampOffset(): time;

  // Get a time in the current domain as specified by timestampOffset.
  toDomainTime(ts: time): time;
}
