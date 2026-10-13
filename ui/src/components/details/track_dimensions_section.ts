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
import {NUM, NUM_NULL, STR, STR_NULL} from '../../trace_processor/query_result';

/**
 * Shows the *effective* dimensions of the track the selected event is on.
 *
 * This is the details-panel half of the dimension presentation layer: it reads
 * the same trace processor rows (`track_dimension`) as the track subtitles do,
 * so the timeline and the details panel can never disagree. Unlike subtitles,
 * no collapse rule is applied here: when you have asked for the details of one
 * event you want its full identity, even the parts which are uniform across
 * the trace.
 *
 * The section is only shown if the track has at least one custom dimension:
 * the well known ones alone (machine, process, thread, ...) are already shown
 * elsewhere in the details panel.
 */
export class TrackDimensionsSection implements TrackEventDetailsPanelSection {
  private dimensions: Dimension[] = [];

  constructor(private readonly trace: Trace) {}

  async load(selection: TrackEventSelection): Promise<void> {
    this.dimensions = [];
    if (!(await hasCustomDimensions(this.trace))) {
      return;
    }
    // Resolve the dimensions of the track the selected event lives on. The
    // track comes from the event itself rather than from the UI track, because
    // one UI track can multiplex several trace processor tracks. The values of
    // well known dimensions are trace processor ids (ucpu, ugpu, upid, utid,
    // machine id): show the numbers users know instead (CPU and GPU numbers,
    // pids, tids, machine label indexes), with names from the corresponding
    // tables as display names.
    const res = await this.trace.engine.query(`
      SELECT DISTINCT
        d.name,
        CASE
          WHEN NOT d.is_well_known THEN d.int_value
          WHEN d.name = 'cpu' THEN (SELECT cpu FROM cpu WHERE id = d.int_value)
          WHEN d.name = 'gpu' THEN (SELECT gpu FROM gpu WHERE id = d.int_value)
          WHEN d.name = 'process'
            THEN (SELECT pid FROM process WHERE upid = d.int_value)
          WHEN d.name = 'thread'
            THEN (SELECT tid FROM thread WHERE utid = d.int_value)
          WHEN d.name = 'machine'
            THEN (SELECT label_index FROM machine WHERE id = d.int_value)
        END AS int_value,
        d.string_value,
        CASE
          WHEN NOT d.is_well_known THEN d.display_name
          WHEN d.name = 'gpu' THEN (SELECT name FROM gpu WHERE id = d.int_value)
          WHEN d.name = 'process'
            THEN (SELECT name FROM process WHERE upid = d.int_value)
          WHEN d.name = 'thread'
            THEN (SELECT name FROM thread WHERE utid = d.int_value)
          WHEN d.name = 'machine'
            THEN (SELECT name FROM machine WHERE id = d.int_value)
        END AS display_name,
        d.is_well_known
      FROM track_dimension AS d
      WHERE d.track_id = (
        SELECT track_id FROM slice WHERE id = ${selection.eventId}
      )
      ORDER BY d.is_well_known DESC, d.name
    `);
    const it = res.iter({
      name: STR,
      int_value: NUM_NULL,
      string_value: STR_NULL,
      display_name: STR_NULL,
      is_well_known: NUM,
    });
    let hasCustom = false;
    for (; it.valid(); it.next()) {
      hasCustom = hasCustom || it.is_well_known === 0;
      this.dimensions.push({
        name: it.name,
        intValue: it.int_value,
        stringValue: it.string_value,
        displayName: it.display_name,
      });
    }
    if (!hasCustom) {
      this.dimensions = [];
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

// Whether the trace has any custom dimension, looked up once per trace: almost
// no trace has any, and the section is loaded on every slice selection.
const customDimensionsByTrace = new WeakMap<Trace, Promise<boolean>>();

function hasCustomDimensions(trace: Trace): Promise<boolean> {
  let result = customDimensionsByTrace.get(trace);
  if (result === undefined) {
    result = trace.engine
      .query(
        `SELECT EXISTS(
          SELECT 1 FROM track_dimension WHERE NOT is_well_known
        ) AS has_custom`,
      )
      .then((res) => res.firstRow({has_custom: NUM}).has_custom !== 0);
    customDimensionsByTrace.set(trace, result);
  }
  return result;
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
