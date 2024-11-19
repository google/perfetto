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
import {Engine} from '../../trace_processor/engine';
import {LONG, NUM} from '../../trace_processor/query_result';
import {VegaView} from '../../widgets/vega_view';

const INPUT_CATEGORY = 'Input';
const PRESENTED_CATEGORY = 'Presented';
const PRESENTED_JANKY_CATEGORY = 'Presented with Predictor Jank';

interface ScrollDeltaPlotDatum {
  // What type of data this is - input scroll or presented scroll. This is used
  // to denote the color of the data point.
  category: string;
  offset: number;
  scrollUpdateId: number;
  ts: number;
  delta: number;
  predictorJank: string;
}

export interface ScrollDeltaDetails {
  ts: time;
  scrollUpdateId: number;
  scrollDelta: number;
  scrollOffset: number;
  predictorJank: number;
}

export interface JankIntervalPlotDetails {
  start_ts: number;
  end_ts: number;
}

export async function getInputScrollDeltas(
  engine: Engine,
  scrollId: number,
): Promise<ScrollDeltaDetails[]> {
  const queryResult = await engine.query(`
    INCLUDE PERFETTO MODULE chrome.scroll_jank.scroll_offsets;

    SELECT
      ts,
      IFNULL(scroll_update_id, 0) AS scrollUpdateId,
      delta_y AS deltaY,
      relative_offset_y AS offsetY
    FROM chrome_scroll_input_offsets
    WHERE scroll_id = ${scrollId};
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
      scrollUpdateId: it.scrollUpdateId,
      scrollOffset: it.offsetY,
      scrollDelta: it.deltaY,
      predictorJank: 0,
    });
  }

  return deltas;
}

export async function getPresentedScrollDeltas(
  engine: Engine,
  scrollId: number,
): Promise<ScrollDeltaDetails[]> {
  const queryResult = await engine.query(`
    INCLUDE PERFETTO MODULE chrome.scroll_jank.scroll_offsets;

    SELECT
      ts,
      IFNULL(scroll_update_id, 0) AS scrollUpdateId,
      delta_y AS deltaY,
      relative_offset_y AS offsetY
    FROM chrome_presented_scroll_offsets
    WHERE scroll_id = ${scrollId}
      AND delta_y IS NOT NULL;
  `);

  const it = queryResult.iter({
    ts: LONG,
    scrollUpdateId: NUM,
    deltaY: NUM,
    offsetY: NUM,
  });
  const deltas: ScrollDeltaDetails[] = [];
  let offset = 0;

  for (; it.valid(); it.next()) {
    offset = it.offsetY;

    deltas.push({
      ts: Time.fromRaw(it.ts),
      scrollUpdateId: it.scrollUpdateId,
      scrollOffset: offset,
      scrollDelta: it.deltaY,
      predictorJank: 0,
    });
  }

  return deltas;
}

export async function getPredictorJankDeltas(
  engine: Engine,
  scrollId: number,
): Promise<ScrollDeltaDetails[]> {
  const queryResult = await engine.query(`
    INCLUDE PERFETTO MODULE chrome.scroll_jank.predictor_error;

    SELECT
      present_ts AS ts,
      IFNULL(scroll_update_id, 0) AS scrollUpdateId,
      delta_y AS deltaY,
      relative_offset_y AS offsetY,
      predictor_jank AS predictorJank
    FROM chrome_predictor_error
    WHERE scroll_id = ${scrollId}
      AND predictor_jank != 0 AND predictor_jank IS NOT NULL;
  `);

  const it = queryResult.iter({
    ts: LONG,
    scrollUpdateId: NUM,
    deltaY: NUM,
    offsetY: NUM,
    predictorJank: NUM,
  });
  const deltas: ScrollDeltaDetails[] = [];
  let offset = 0;

  for (; it.valid(); it.next()) {
    offset = it.offsetY;

    deltas.push({
      ts: Time.fromRaw(it.ts),
      scrollUpdateId: it.scrollUpdateId,
      scrollOffset: offset,
      scrollDelta: it.deltaY,
      predictorJank: it.predictorJank,
    });
  }

  return deltas;
}

export async function getJankIntervals(
  engine: Engine,
  startTs: time,
  dur: duration,
): Promise<JankIntervalPlotDetails[]> {
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

// TODO(b/352038635): Show the error margin on the graph - what the pixel offset
// should have been if there were no predictor jank.
export function buildScrollOffsetsGraph(
  inputDeltas: ScrollDeltaDetails[],
  presentedDeltas: ScrollDeltaDetails[],
  predictorDeltas: ScrollDeltaDetails[],
  jankIntervals: JankIntervalPlotDetails[],
): m.Child {
  const inputData = buildOffsetData(inputDeltas, INPUT_CATEGORY);
  // Filter out the predictor deltas from the presented deltas, as these will be
  // rendered in a new layer, with new tooltip/color/etc.
  const filteredPresentedDeltas = presentedDeltas.filter((item) => {
    for (let i = 0; i < predictorDeltas.length; i++) {
      const predictorDelta: ScrollDeltaDetails = predictorDeltas[i];
      if (
        predictorDelta.ts == item.ts &&
        predictorDelta.scrollUpdateId == item.scrollUpdateId
      ) {
        return false;
      }
    }
    return true;
  });

  const presentedData = buildOffsetData(
    filteredPresentedDeltas,
    PRESENTED_CATEGORY,
  );
  const predictorData = buildOffsetData(
    predictorDeltas,
    PRESENTED_JANKY_CATEGORY,
  );
  const jankData = buildJankLayerData(jankIntervals);

  return m(VegaView, {
    spec: `
{
  "$schema": "https://vega.github.io/schema/vega-lite/v5.json",
  "description": "Scatter plot showcasing the pixel offset deltas between input frames and presented frames.",
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
            "domain": [
              "${INPUT_CATEGORY}",
              "${PRESENTED_CATEGORY}",
              "${PRESENTED_JANKY_CATEGORY}"
            ],
            "range": ["blue", "red", "orange"]
          },
          "legend": {
            "title":null
          }
        },
        "tooltip": [
          {
            "field": "delta",
            "type": "quantitative",
            "title": "Delta",
            "format": ".2f"
          },
          {
            "field": "scrollUpdateId",
            "type": "quantititive",
            "title": "Trace Id"
          },
          {
            "field": "predictorJank",
            "type": "nominal",
            "title": "Predictor Jank"
          }
        ]
      }
    }
  ]
}
`,
    data: {table: inputData.concat(presentedData).concat(predictorData)},
  });
}

function buildOffsetData(
  deltas: ScrollDeltaDetails[],
  category: string,
): ScrollDeltaPlotDatum[] {
  const plotData: ScrollDeltaPlotDatum[] = [];
  for (const delta of deltas) {
    let predictorJank = 'N/A';
    if (delta.predictorJank > 0) {
      predictorJank = parseFloat(delta.predictorJank.toString()).toFixed(2);
      predictorJank +=
        " (times delta compared to the next/previous frame's delta)";
    }
    plotData.push({
      category: category,
      ts: Number(delta.ts) / 10e8,
      scrollUpdateId: delta.scrollUpdateId,
      offset: delta.scrollOffset,
      delta: delta.scrollDelta,
      predictorJank: predictorJank,
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
