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

import m from 'mithril';

import {duration, Time, time} from '../../base/time';
import {EngineProxy} from '../../common/engine';
import {LONG, NUM, STR} from '../../common/query_result';
import {VegaView} from '../../frontend/widgets/vega_view';

const USER_CATEGORY = 'User';
const APPLIED_CATEGORY = 'Applied';

interface ScrollDeltaPlotDatum {
  // What type of data this is - user scroll or applied scroll. This is used
  // to denote the color of the data point.
  category: string;
  offset: number;
  scrollUpdateIds: string;
  ts: number;
  delta: number;
}

export interface ScrollDeltaDetails {
  ts: time;
  scrollUpdateIds: string;
  scrollDelta: number;
  scrollOffset: number;
}

export interface JankIntervalPlotDetails {
  start_ts: number;
  end_ts: number;
}

export async function getUserScrollDeltas(
    engine: EngineProxy, startTs: time, dur: duration):
    Promise<ScrollDeltaDetails[]> {
  const queryResult = await engine.query(`
    INCLUDE PERFETTO MODULE chrome.scroll_jank.scroll_offsets;

    SELECT
      ts,
      IFNULL(scroll_update_id, "") AS scrollUpdateId,
      delta_y AS deltaY,
      offset_y AS offsetY
    FROM chrome_scroll_input_offsets
    WHERE ts >= ${startTs} AND ts <= ${startTs + dur};
  `);

  const it = queryResult.iter({
    ts: LONG,
    scrollUpdateId: NUM,
    deltaY: NUM,
    offsetY: NUM,
  });
  const deltas: ScrollDeltaDetails[] = [];

  for (; it.valid(); it.next()) {
    deltas.push({
      ts: Time.fromRaw(it.ts),
      scrollUpdateIds: it.scrollUpdateId.toString(),
      scrollOffset: it.offsetY,
      scrollDelta: it.deltaY,
    });
  }

  return deltas;
}

export async function getAppliedScrollDeltas(
    engine: EngineProxy, startTs: time, dur: duration):
    Promise<ScrollDeltaDetails[]> {
  const queryResult = await engine.query(`
    INCLUDE PERFETTO MODULE chrome.scroll_jank.scroll_offsets;

    WITH scroll_update_ids AS (
      SELECT DISTINCT
        ts,
        GROUP_CONCAT(scroll_update_id, ', ')
          OVER (PARTITION BY ts) AS scroll_update_ids
      FROM chrome_presented_scroll_offsets
    )
    SELECT
      ts,
      IFNULL(scroll_update_ids, "") AS scrollUpdateIds,
      delta_y AS deltaY,
      offset_y AS offsetY
    FROM chrome_presented_scroll_offsets
    LEFT JOIN scroll_update_ids
      USING(ts)
    WHERE ts >= ${startTs} AND ts <= ${startTs + dur}
      AND delta_y IS NOT NULL;
  `);

  const it = queryResult.iter({
    ts: LONG,
    scrollUpdateIds: STR,
    deltaY: NUM,
    offsetY: NUM,
  });
  const deltas: ScrollDeltaDetails[] = [];
  let offset = 0;

  for (; it.valid(); it.next()) {
    offset = it.offsetY;

    deltas.push({
      ts: Time.fromRaw(it.ts),
      scrollUpdateIds: it.scrollUpdateIds,
      scrollOffset: offset,
      scrollDelta: it.deltaY,
    });
  }

  return deltas;
}

export async function getJankIntervals(
    engine: EngineProxy, startTs: time, dur: duration):
    Promise<JankIntervalPlotDetails[]> {
  const queryResult = await engine.query(`
    INCLUDE PERFETTO MODULE chrome.scroll_jank.scroll_jank_intervals;

    SELECT
      ts,
      dur
    FROM chrome_janky_frame_presentation_intervals
    WHERE ts >= ${startTs} AND ts <= ${startTs + dur};
  `);

  const it = queryResult.iter({
    ts: LONG,
    dur: LONG,
  });

  const details: JankIntervalPlotDetails[] = [];

  for (; it.valid(); it.next()) {
    details.push({
      start_ts: Number(it.ts),
      end_ts: Number(it.ts + it.dur),
    });
  }

  return details;
}

export function buildScrollOffsetsGraph(
    userDeltas: ScrollDeltaDetails[],
    appliedDeltas: ScrollDeltaDetails[],
    jankIntervals: JankIntervalPlotDetails[]): m.Child {
  const userData = buildOffsetData(userDeltas, USER_CATEGORY);
  const appliedData = buildOffsetData(appliedDeltas, APPLIED_CATEGORY);
  const jankData = buildJankLayerData(jankIntervals);

  return m(VegaView, {
    spec: `
{
  "$schema": "https://vega.github.io/schema/vega-lite/v5.json",
  "description": "Scatter plot showcasing the pixel offset deltas between user scrolling and applied scrolling.",
  "width": "container",
  "height": 200,
  "padding": 5,

  "data": {
    "name": "table"
  },

  "layer": [
    {
      "mark": "rect",
      "data": {
        "values": [
          ${jankData}
        ]
      },
      "encoding": {
        "x": {
          "field": "start",
          "type": "quantitative"
        },
        "x2": {
          "field": "end",
          "type": "quantitative"
        },
        "color": {
          "value": "#D3D3D3"
        }
      }
    },
    {
      "mark": {
        "type": "point",
        "filled": true
      },

      "encoding": {
        "x": {
          "field": "ts",
          "type": "quantitative",
          "title": "Raw Timestamp",
          "axis" : {
            "labels": true
          },
          "scale": {"zero":false}
        },
        "y": {
          "field": "offset",
          "type": "quantitative",
          "title": "Offset (pixels)",
          "scale": {"zero":false}
        },
        "color": {
          "field": "category",
          "type": "nominal",
          "scale": {
            "domain": ["${USER_CATEGORY}", "${APPLIED_CATEGORY}"],
            "range": ["blue", "red"]
          },
          "legend": {
            "title":null
          }
        },
        "tooltip": [
          {"field": "delta", "type": "quantitative", "title": "Delta"},
          {"field": "scrollUpdateIds", "type": "nominal", "title": "Trace Ids"}
        ]
      }
    }
  ]
}
`,
    data: {table: userData.concat(appliedData)},
  });
}

function buildOffsetData(
    deltas: ScrollDeltaDetails[], category: string): ScrollDeltaPlotDatum[] {
  const plotData: ScrollDeltaPlotDatum[] = [];
  for (const delta of deltas) {
    plotData.push({
      category: category,
      ts: Number(delta.ts) / 10e8,
      scrollUpdateIds: delta.scrollUpdateIds,
      offset: delta.scrollOffset,
      delta: delta.scrollDelta,
    });
  }

  return plotData;
}

function buildJankLayerData(janks: JankIntervalPlotDetails[]): string {
  let dataJsonString = '';
  for (let i = 0; i < janks.length; i++) {
    if (i != 0) {
      dataJsonString += ',';
    }
    const jank = janks[i];
    dataJsonString += `
    {
      "start": ${jank.start_ts / 10e8},
      "end": ${jank.end_ts / 10e8}
    }
    `;
  }
  return dataJsonString;
}
