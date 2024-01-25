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

import {HSLColor} from '../../common/color';
import {makeColorScheme} from '../../common/colorizer';
import {NamedRow, NamedSliceTrack} from '../../frontend/named_slice_track';
import {SLICE_LAYOUT_FIT_CONTENT_DEFAULTS} from '../../frontend/slice_layout';
import {EngineProxy, Slice} from '../../public';

const GREEN = makeColorScheme(new HSLColor('#4CAF50'));  // Green 500

export class ExpectedFramesTrack extends NamedSliceTrack {
  constructor(
    engine: EngineProxy, maxDepth: number, trackKey: string,
      private trackIds: number[]) {
    super({engine, trackKey});
    this.sliceLayout = {
      ...SLICE_LAYOUT_FIT_CONTENT_DEFAULTS,
      depthGuess: maxDepth,
    };
  }

  getSqlSource(): string {
    return `
      SELECT
        ts,
        dur,
        layout_depth as depth,
        name,
        id
      from experimental_slice_layout
      where
        filter_track_ids = '${this.trackIds.join(',')}'
    `;
  }

  rowToSlice(row: NamedRow): Slice {
    const baseSlice = super.rowToSlice(row);
    return {...baseSlice, colorScheme: GREEN};
  }
}
