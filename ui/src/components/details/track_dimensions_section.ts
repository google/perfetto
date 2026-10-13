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
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {NUM, STR, STR_NULL} from '../../trace_processor/query_result';

interface DimensionDisplay {
  readonly name: string;
  readonly displayValue: string | null;
  readonly displayName: string | null;
}

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
  private dimensions: DimensionDisplay[] = [];

  constructor(private readonly trace: Trace) {}

  async load(selection: TrackEventSelection): Promise<void> {
    this.dimensions = [];
    // The section is optional: a failure, e.g. with a trace processor which
    // doesn't have `track_dimension`, must not break the rest of the details.
    try {
      if (await hasCustomDimensions(this.trace)) {
        this.dimensions = await queryDimensions(this.trace, selection.eventId);
      }
    } catch {
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

// The effective dimensions of the track |eventId| is on, or none if it has no
// custom dimension (see TrackDimensionsSection).
async function queryDimensions(
  trace: Trace,
  eventId: number,
): Promise<DimensionDisplay[]> {
  const dimensions: DimensionDisplay[] = [];
  // One UI track can multiplex several trace processor tracks, so use the
  // track of the selected event. Presentation values come from stdlib.
  const res = await trace.engine.query(`
    SELECT
      d.name,
      d.display_value,
      d.display_name,
      d.is_well_known
    FROM track_dimension AS d
    WHERE d.track_id = (
      SELECT track_id FROM slice WHERE id = ${eventId}
    )
    ORDER BY d.is_well_known DESC, d.name
  `);
  const it = res.iter({
    name: STR,
    display_value: STR_NULL,
    display_name: STR_NULL,
    is_well_known: NUM,
  });
  let hasCustom = false;
  for (; it.valid(); it.next()) {
    hasCustom = hasCustom || it.is_well_known === 0;
    dimensions.push({
      name: it.name,
      displayValue: it.display_value,
      displayName: it.display_name,
    });
  }
  return hasCustom ? dimensions : [];
}

// Whether the trace has custom dimensions and supports their presentation
// columns, looked up once per trace rather than on every slice selection.
const customDimensionsByTrace = new WeakMap<Trace, Promise<boolean>>();

function hasCustomDimensions(trace: Trace): Promise<boolean> {
  let result = customDimensionsByTrace.get(trace);
  if (result === undefined) {
    result = trace.engine
      .query(
        `SELECT EXISTS(
          SELECT display_value FROM track_dimension WHERE is_well_known = 0
        ) AS has_custom`,
      )
      .then((res) => res.firstRow({has_custom: NUM}).has_custom !== 0)
      // Includes failures to decode a response from an older trace processor.
      .catch(() => false);
    customDimensionsByTrace.set(trace, result);
  }
  return result;
}

function renderValue(dimension: DimensionDisplay): string {
  const value = dimension.displayValue ?? '';
  const displayName = dimension.displayName;
  if (displayName !== undefined && displayName !== null && displayName !== '') {
    return `${value} (${displayName})`;
  }
  return value;
}
