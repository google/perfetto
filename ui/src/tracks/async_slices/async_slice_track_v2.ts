// Copyright (C) 2023 The Android Open Source Project
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

import {NamedSliceTrack} from '../../frontend/named_slice_track';
import {SLICE_LAYOUT_FIT_CONTENT_DEFAULTS} from '../../frontend/slice_layout';
import {NewTrackArgs} from '../../frontend/track';
import {Slice} from '../../public';

export class AsyncSliceTrackV2 extends NamedSliceTrack {
  constructor(
      args: NewTrackArgs, maxDepth: number, private trackIds: number[]) {
    super(args);
    this.sliceLayout = {
      ...SLICE_LAYOUT_FIT_CONTENT_DEFAULTS,
      depthGuess: maxDepth,
    };
  }

  getSqlSource(): string {
    return `
    select
      ts,
      dur,
      layout_depth as depth,
      ifnull(name, '[null]') as name,
      id,
      thread_dur as threadDur
    from experimental_slice_layout
    where filter_track_ids = '${this.trackIds.join(',')}'
    `;
  }

  onUpdatedSlices(slices: Slice[]) {
    for (const slice of slices) {
      slice.isHighlighted = (slice === this.hoveredSlice);
    }
  }
}
