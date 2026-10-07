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

import {vi} from 'vitest';
import {defer} from '../../base/deferred';
import {SliceTrack} from '../../components/tracks/slice_track';
import type {Trace} from '../../public/trace';
import type {TrackRenderContext} from '../../public/track';
import {CallstackTrack} from './callstack_track';

describe('CallstackTrack', () => {
  afterEach(() => vi.restoreAllMocks());

  function setup(source = 'linux.perf') {
    const ready = defer<void>();
    const query = vi.fn(async (_sql: string) => {
      await ready;
      return {iter: () => ({valid: () => false})};
    });
    const trace = {
      engine: {query},
      raf: {scheduleFullRedraw: vi.fn()},
    } as unknown as Trace;
    const track = new CallstackTrack(trace, 'test', {
      source,
      utid: 1,
      upid: 2,
      sessionId: 3,
    });
    const context = {} as TrackRenderContext;
    return {track, query, ready, context};
  }

  test('prepares lazily once for concurrent render and selection requests', async () => {
    const render = vi
      .spyOn(SliceTrack.prototype, 'render')
      .mockImplementation(() => {});
    const {track, query, ready, context} = setup();
    expect(query).not.toHaveBeenCalled();
    expect(track.getDataset()).toBeUndefined();
    track.render(context);
    track.render(context);
    const first = track.getSelectionDetails(1);
    const second = track.getSelectionDetails(2);
    expect(query).toHaveBeenCalledOnce();
    expect(render).not.toHaveBeenCalled();
    ready.resolve();
    await Promise.all([first, second]);
    expect(query).toHaveBeenCalledTimes(7); // Four modules, one runs table, two selections.
    expect(track.getDataset()).toBeDefined();
    const sql = query.mock.calls.map(([statement]) => statement).join('\n');
    expect(sql).toContain('ss.session_id = 3');
    expect(sql).not.toContain('_samples as');
    track.render(context);
    expect(render).toHaveBeenCalledWith(context);
    expect(query).toHaveBeenCalledTimes(7);
  });

  test('sources with the same sanitized name keep separate run tables', async () => {
    const first = setup('custom.cpu');
    const second = setup('custom_cpu');
    first.ready.resolve();
    second.ready.resolve();
    await Promise.all([
      first.track.getSelectionDetails(1),
      second.track.getSelectionDetails(1),
    ]);
    const firstDataset = first.track.getDataset();
    const secondDataset = second.track.getDataset();
    expect(firstDataset).toBeDefined();
    expect(secondDataset).toBeDefined();
    expect(firstDataset!.src).not.toBe(secondDataset!.src);
  });

  test('preparation errors reach selections and the render error path without retrying', async () => {
    const {track, query, ready, context} = setup();
    track.render(context);
    const first = expect(track.getSelectionDetails(1)).rejects.toThrow(
      'prepare failed',
    );
    const second = expect(track.getSelectionDetails(2)).rejects.toThrow(
      'prepare failed',
    );
    ready.reject(new Error('prepare failed'));
    await Promise.all([first, second]);
    await vi.waitFor(() =>
      expect(() => track.render(context)).toThrow('prepare failed'),
    );
    expect(query).toHaveBeenCalledOnce();
    expect(track.getDataset()).toBeUndefined();
  });
});
