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
import {generateRenderQuery} from './slice_track';

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
