// Copyright (C) 2026 The Android Open Source Project
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

import m from 'mithril';
import type {Trace} from '../../public/trace';
import type {TrackEventSelection} from '../../public/selection';
import type {TrackEventDetailsPanelSection} from './thread_slice_details_tab';
import {type Dimension, dimensionValue} from '../../public/dimensions';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {NUM_NULL, STR, STR_NULL} from '../../trace_processor/query_result';

/**
 * Shows the *effective* dimensions of the track the selected event is on.
 *
 * This is the details-panel half of the dimension presentation layer: it reads
 * exactly the same trace processor rows as the track subtitles do, so the
 * timeline and the details panel can never disagree. Unlike subtitles, no
 * collapse rule is applied here: when you have asked for the details of one
 * event you want its full identity, even the parts which are uniform across
 * the trace.
 */
export class TrackDimensionsSection implements TrackEventDetailsPanelSection {
  private dimensions: Dimension[] = [];

  constructor(private readonly trace: Trace) {}

  async load(selection: TrackEventSelection): Promise<void> {
    this.dimensions = [];
    // Resolve the dimensions of the track the selected event lives on. The
    // track comes from the event itself rather than from the UI track, because
    // one UI track can multiplex several trace processor tracks.
    const res = await this.trace.engine.query(`
      SELECT DISTINCT name, int_value, string_value, display_name
      FROM track_dimension
      WHERE track_id = (SELECT track_id FROM slice WHERE id = ${selection.eventId})
      ORDER BY is_well_known DESC, name
    `);
    const it = res.iter({
      name: STR,
      int_value: NUM_NULL,
      string_value: STR_NULL,
      display_name: STR_NULL,
    });
    for (; it.valid(); it.next()) {
      this.dimensions.push({
        name: it.name,
        intValue: it.int_value,
        stringValue: it.string_value,
        displayName: it.display_name,
      });
    }
  }

  render(): m.Children {
    if (this.dimensions.length === 0) return null;
    return m(
      Section,
      {title: 'Dimensions'},
      m(
        Tree,
        this.dimensions.map((dimension) =>
          m(TreeNode, {
            left: dimension.name,
            right: renderValue(dimension),
          }),
        ),
      ),
    );
  }
}

// Shows the canonical value, with the optional display name next to it: joins
// and merging use the canonical value, so it must stay visible.
function renderValue(dimension: Dimension): string {
  const value = dimensionValue(dimension);
  const displayName = dimension.displayName;
  if (displayName !== undefined && displayName !== null && displayName !== '') {
    return `${value} (${displayName})`;
  }
  return value;
}
