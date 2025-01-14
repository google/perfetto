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

import {
  NAMED_ROW,
  NamedSliceTrack,
} from '../../components/tracks/named_slice_track';
import {Slice} from '../../public/track';
import {Trace} from '../../public/trace';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {TrackEventSelection} from '../../public/selection';
import {NUM} from '../../trace_processor/query_result';
import {ColorScheme} from '../../base/color_scheme';
import {JANK_COLOR} from './jank_colors';
import {makeColorScheme} from '../../components/colorizer';
import {HSLColor} from '../../base/color';
import {ScrollTimelineDetailsPanel} from './scroll_timeline_details_panel';
import {
  ScrollTimelineModel,
  ScrollUpdateClassification,
} from './scroll_timeline_model';

const INDIGO = makeColorScheme(new HSLColor([231, 48, 48]));
const GRAY = makeColorScheme(new HSLColor([0, 0, 62]));
const DARK_GREEN = makeColorScheme(new HSLColor([120, 44, 34]));
const TEAL = makeColorScheme(new HSLColor([187, 90, 42]));

function toColorScheme(
  classification: ScrollUpdateClassification,
): ColorScheme | undefined {
  switch (classification) {
    case ScrollUpdateClassification.DEFAULT:
      return INDIGO;
    case ScrollUpdateClassification.JANKY:
      return JANK_COLOR;
    case ScrollUpdateClassification.COALESCED:
      return GRAY;
    case ScrollUpdateClassification.FIRST_SCROLL_UPDATE_IN_FRAME:
      return DARK_GREEN;
    case ScrollUpdateClassification.INERTIAL:
      return TEAL;
    case ScrollUpdateClassification.STEP:
      return undefined;
  }
}

const SCROLL_TIMELINE_TRACK_ROW = {
  ...NAMED_ROW,
  classification: NUM,
};
type ScrollTimelineTrackRow = typeof SCROLL_TIMELINE_TRACK_ROW;

export class ScrollTimelineTrack extends NamedSliceTrack<
  Slice,
  ScrollTimelineTrackRow
> {
  /**
   * Constructs a scroll timeline track for a given `trace`.
   *
   * @param trace - The trace whose data the track will display
   * @param model - A model of the scroll timeline.
   */
  constructor(
    trace: Trace,
    private readonly model: ScrollTimelineModel,
  ) {
    super(trace, model.trackUri);
  }

  override getSqlSource(): string {
    return `SELECT * FROM ${this.model.tableName}`;
  }

  override getRowSpec(): ScrollTimelineTrackRow {
    return SCROLL_TIMELINE_TRACK_ROW;
  }

  override rowToSlice(row: ScrollTimelineTrackRow): Slice {
    const baseSlice = super.rowToSliceBase(row);
    const colorScheme = toColorScheme(row.classification);
    if (colorScheme === undefined) {
      return baseSlice;
    } else {
      return {...baseSlice, colorScheme};
    }
  }

  override detailsPanel(sel: TrackEventSelection): TrackEventDetailsPanel {
    return new ScrollTimelineDetailsPanel(this.trace, this.model, sel.eventId);
  }
}
