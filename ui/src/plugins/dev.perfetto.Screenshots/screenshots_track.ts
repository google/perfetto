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

import m from 'mithril';
import {QuerySlot} from '../../base/query_slot';
import {materialColorScheme} from '../../components/colorizer';
import {SliceTrack} from '../../components/tracks/slice_track';
import type {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {ScreenshotDetailsPanel} from './screenshot_panel';

export function createScreenshotsTrack(trace: Trace, uri: string) {
  const imageSlot = new QuerySlot<string>();

  // Screenshot slices are instants (0 dur), but we want the tooltip to show up
  // not only when hovering over the instant event exactly, but also when
  // hovering the area to the right of it, up until the next screenshot.
  //
  // To achieve this, we set dur to -1 and force depth to 0, which renders the
  // slices as incomplete slices: a rect from the screenshot timestamp up until
  // the timestamp of the following screenshot, fading out to the right. The
  // fade indicates that the screenshot is correct at the start of the slice and
  // is simply extrapolated towards the end, as opposed to a fully opaque slice
  // which would imply homogeneity across the entire span.
  //
  // ROW_NUMBER() gives each slice a monotonically increasing name, which is
  // also used for colorization so adjacent slices get contrasting colors.
  //
  // Note: we could use `COALESCE(LEAD(ts) OVER (ORDER BY ts) - ts, -1) AS dur`
  // for solid rectangles instead of fading ones.
  const query = `
    SELECT
      id,
      ts,
      '📷 ' || CAST(ROW_NUMBER() OVER (ORDER BY ts) AS TEXT) AS name,
      -1 AS dur,
      0 AS depth
    FROM android_screenshots
  `;
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
      src: query,
    }),
    detailsPanel: () => {
      return new ScreenshotDetailsPanel(trace.engine);
    },
    colorizer: (row) => {
      return materialColorScheme(row.name);
    },
    tooltip: (data) => {
      const screenshot = imageSlot.use({
        key: {id: data.id},
        retainOn: ['id'],
        queryFn: async () => {
          const result = await trace.engine.query(`
            select extract_arg(arg_set_id, 'screenshot.jpg_image') as image_data
            from slice
            where id = ${data.id}
          `);
          const row = result.firstRow({image_data: STR});
          return row.image_data;
        },
      });

      if (screenshot.data) {
        return [
          m('img.pf-screenshot-tooltip__img', {
            src: 'data:image/png;base64, ' + screenshot.data,
          }),
        ];
      } else {
        return 'Loading...';
      }
    },
  });
}
