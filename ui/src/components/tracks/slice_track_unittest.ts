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

import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, LONG_NULL, NUM} from '../../trace_processor/query_result';
import {generateRenderQuery, SliceTrack} from './slice_track';
import {vi} from 'vitest';
import {Time} from '../../base/time';
import {HighPrecisionTime} from '../../base/high_precision_time';
import {HighPrecisionTimeSpan} from '../../base/high_precision_time_span';
import {TimeScale} from '../../base/time_scale';
import type {Trace} from '../../public/trace';
import {GRAY} from '../colorizer';

describe('generateRenderQuery', () => {
  test('minimal query', () => {
    const dataset = new SourceDataset({
      src: 'foo',
      schema: {ts: LONG},
    });
    expect(generateRenderQuery(dataset)).toBe(
      `SELECT ts AS ts, ROW_NUMBER() OVER (ORDER BY ts) AS id, 0 AS layer, 0 AS depth, 0 AS dur FROM (${dataset.query()})`,
    );
  });

  test('full query', () => {
    const dataset = new SourceDataset({
      src: 'foo',
      schema: {id: NUM, ts: LONG, dur: LONG, depth: NUM, layer: NUM},
    });
    expect(generateRenderQuery(dataset)).toBe(
      `SELECT id AS id, ts AS ts, dur AS dur, depth AS depth, layer AS layer FROM (${dataset.query()})`,
    );
  });

  test('no dur, no depth', () => {
    const dataset = new SourceDataset({
      src: 'foo',
      schema: {id: NUM, ts: LONG, layer: NUM},
    });
    expect(generateRenderQuery(dataset)).toBe(
      `SELECT id AS id, ts AS ts, layer AS layer, 0 AS depth, 0 AS dur FROM (${dataset.query()})`,
    );
  });

  test('no depth', () => {
    const dataset = new SourceDataset({
      src: 'foo',
      schema: {id: NUM, ts: LONG, layer: NUM, dur: LONG},
    });
    expect(generateRenderQuery(dataset)).toBe(
      `SELECT id AS id, ts AS ts, layer AS layer, dur AS dur, internal_layout(ts, dur) OVER (ORDER BY ts ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS depth FROM (${dataset.query()})`,
    );
  });

  test('nullable dur, do depth', () => {
    const dataset = new SourceDataset({
      src: 'foo',
      schema: {id: NUM, ts: LONG, layer: NUM, dur: LONG_NULL},
    });
    expect(generateRenderQuery(dataset)).toBe(
      `SELECT id AS id, ts AS ts, layer AS layer, COALESCE(dur, -1) AS dur, internal_layout(ts, COALESCE(dur, -1)) OVER (ORDER BY ts ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS depth FROM (${dataset.query()})`,
    );
  });
});

describe('SliceTrack compressed row interactions', () => {
  const timescale = new TimeScale(
    new HighPrecisionTimeSpan(new HighPrecisionTime(Time.ZERO), 100),
    {left: 0, right: 100},
  );

  function setup(collapsed = true, customClick = false) {
    const selectTrackEvent = vi.fn();
    const onSliceClick = vi.fn();
    const trace = {
      timeline: {},
      selection: {selectTrackEvent},
      raf: {scheduleFullRedraw: vi.fn()},
    } as unknown as Trace;
    const track = SliceTrack.create({
      trace,
      uri: 'test',
      dataset: new SourceDataset({src: 'slices', schema: {ts: LONG}}),
      initialMaxDepth: 1,
      sliceLayout: {collapsed},
      ...(customClick && {onSliceClick}),
    });
    // Seed fetched data to exercise actual hit testing without a SQL engine.
    Object.assign(track, {
      currentDataFrame: {
        start: Time.ZERO,
        end: Time.fromRaw(100n),
        slices: {
          starts: new Float32Array([0, 10]),
          ends: new Float32Array([100, 90]),
          depths: new Uint16Array([0, 1]),
          patterns: new Uint8Array(2),
          slices: [0, 1].map((id) => ({
            id,
            title: `Frame ${id}`,
            subtitle: '',
            count: 1,
            colorScheme: GRAY,
            fillRatio: 1,
            row: {ts: 0n},
          })),
          count: 2,
        },
        instants: {count: 0},
      },
    });
    track.getHeight();
    return {track, selectTrackEvent, onSliceClick};
  }

  test('first compressed-row click expands; next click selects', () => {
    const {track, selectTrackEvent} = setup();
    const event = {x: 50, y: 22, timescale};
    track.onMouseMove(event);
    expect(track.renderTooltip()).toBe('Click to expand rows');
    expect(track.onMouseClick(event)).toBe(true);
    expect(selectTrackEvent).not.toHaveBeenCalled();
    expect(track.getHeight()).toBe(42);
    expect(track.getSliceVerticalBounds(1)).toEqual({top: 21, bottom: 39});
    expect(track.onMouseClick(event)).toBe(true);
    expect(selectTrackEvent).toHaveBeenCalledWith('test', 1);
  });

  test.each([10, 21])('top row at y=%s selects without expanding', (y) => {
    const {track, selectTrackEvent} = setup();
    track.onMouseClick({x: 50, y, timescale});
    expect(selectTrackEvent).toHaveBeenCalledWith('test', 0);
    expect(track.getHeight()).toBe(27);
  });

  test('empty space in compressed rows does not expand', () => {
    const {track, selectTrackEvent} = setup();
    expect(track.onMouseClick({x: 5, y: 22, timescale})).toBe(false);
    expect(selectTrackEvent).not.toHaveBeenCalled();
    expect(track.getHeight()).toBe(27);
  });

  test('expansion precedes custom click callbacks', () => {
    const {track, onSliceClick} = setup(true, true);
    const event = {x: 50, y: 22, timescale};
    track.onMouseClick(event);
    expect(onSliceClick).not.toHaveBeenCalled();
    track.getHeight();
    track.onMouseClick(event);
    expect(onSliceClick).toHaveBeenCalledOnce();
  });

  test('expanded rows retain normal click behavior', () => {
    const {track, selectTrackEvent} = setup(false);
    track.onMouseClick({x: 50, y: 30, timescale});
    expect(selectTrackEvent).toHaveBeenCalledWith('test', 1);
    expect(track.getHeight()).toBe(42);
  });
});
