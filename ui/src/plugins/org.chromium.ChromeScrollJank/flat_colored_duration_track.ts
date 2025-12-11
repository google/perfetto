// Copyright (C) 2025 The Android Open Source Project
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
import {HSLColor} from '../../base/color';
import {clamp} from '../../base/math_utils';
import {makeColorScheme} from '../../components/colorizer';
import {Trace} from '../../public/trace';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {DurationWidget} from '../../components/widgets/duration';
import {Timestamp} from '../../components/widgets/timestamp';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {SourceDataset} from '../../trace_processor/dataset';
import {SliceTrack} from '../../components/tracks/slice_track';
import {Time} from '../../base/time';

/*
  This is a custom track which displays the intervals between uniform events
  and colors the durations based on the duration of the interval, focusing
  on the [4ms, 32ms] range.
*/
export function createFlatColoredDurationTrack(
  trace: Trace,
  uri: string,
  sqlSrc: string,
) {
  return SliceTrack.create({
    trace,
    uri,
    dataset: new SourceDataset({
      schema: {
        id: NUM,
        ts: LONG,
        dur: LONG,
        name: STR,
        depth: NUM,
      },
      src: `
        SELECT
          id,
          ts,
          dur,
          printf('%.3fms', dur / 1e6) AS name,
          0 as depth
        FROM (${sqlSrc})
      `,
    }),
    colorizer: (row) => {
      // Use the log2 of the duration in ms as the value, as we want to focus on
      // differentiating between 4ms, 8ms, 16ms and 32ms values.
      const rawValue = Math.log2(Math.max(Number(row.dur) / 1e6, 1));
      // Normalise this to [0, 5] range.
      const value = clamp(rawValue, 1, 6) - 1;
      // 60 offset in hue forces the colors to be visually distinct.
      return makeColorScheme(new HSLColor([60 * value, 80, 70]));
    },
    detailsPanel: (row) => {
      return {
        render() {
          return m(
            Section,
            {title: 'Details'},
            m(
              Tree,
              m(TreeNode, {
                left: 'ID',
                right: row.id,
              }),
              m(TreeNode, {
                left: 'Timestamp',
                right: m(Timestamp, {trace, ts: Time.fromRaw(row.ts)}),
              }),
              m(TreeNode, {
                left: 'Duration',
                right: m(DurationWidget, {trace, dur: row.dur}),
              }),
              // TODO: Consider adding a link to the original event.
            ),
          );
        },
      };
    },
  });
}
