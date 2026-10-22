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
import {SliceTrack} from '../../components/tracks/slice_track';
import type {Trace} from '../../public/trace';
import {CallstackTrack} from './callstack_track';
import {createStackSampleTrack} from './index';
import {GRAY, getColorForSlice} from '../../components/colorizer';
import {sampleColorScheme} from './sample_colors';

describe('stack sample colors', () => {
  afterEach(() => vi.restoreAllMocks());

  test.each([0, 1, 2, 3])(
    'instants and frames share mapping colors for category %i',
    (category) => {
      const create = vi
        .spyOn(SliceTrack, 'create')
        .mockReturnValue({} as ReturnType<typeof SliceTrack.create>);
      const trace = {raf: {scheduleFullRedraw: vi.fn()}} as unknown as Trace;
      createStackSampleTrack(
        trace,
        'samples',
        {source: 'linux.perf', utid: 1},
        undefined,
        () => {},
      );
      const track = new CallstackTrack(trace, 'frames', {
        source: 'linux.perf',
        utid: 1,
        upid: 1,
      });
      track.settings[0].update('mapping');
      const instantColor = create.mock.calls[0][0].colorizer!;
      const frameColor = create.mock.calls[1][0].colorizer!;
      const row = {
        id: 1,
        ts: 10n,
        dur: -1n,
        depth: 0,
        callsiteId: 2,
        frameId: 3,
        category,
        mappingName: '/out/trace_processor_shell',
        sampleCount: 1,
      };
      const expected = sampleColorScheme(category, row.mappingName);
      expect(instantColor({...row, name: ''})).toEqual(expected);
      expect(frameColor({...row, name: 'resolved_function'})).toEqual(expected);
      if (category === 3) expect(expected).toEqual(GRAY);
    },
  );
  test('function coloring is the default and can switch to mapping and back', () => {
    const create = vi
      .spyOn(SliceTrack, 'create')
      .mockReturnValue({} as ReturnType<typeof SliceTrack.create>);
    const scheduleFullRedraw = vi.fn();
    const trace = {raf: {scheduleFullRedraw}} as unknown as Trace;
    const track = new CallstackTrack(trace, 'frames', {
      source: 'linux.perf',
      utid: 1,
      upid: 1,
    });
    const attrs = create.mock.calls[0][0];
    const row = {
      id: 1,
      ts: 10n,
      dur: -1n,
      depth: 0,
      frameId: 1,
      name: 'work123',
      category: 0,
      mappingName: '/bin/program',
      sampleCount: 1,
    };
    const functionKey = attrs.getKey!();
    expect(track.settings[0].value).toBe('function');
    const functionColor = getColorForSlice(row.name, {
      stripTrailingDigits: false,
    });
    expect(attrs.colorizer!(row)).toEqual(functionColor);
    expect(
      attrs.colorizer!({...row, category: 1, mappingName: '/lib/libc.so'}),
    ).toEqual(functionColor);
    expect(scheduleFullRedraw).not.toHaveBeenCalled();
    track.settings[0].update('mapping');
    expect(attrs.getKey!()).not.toBe(functionKey);
    expect(attrs.colorizer!(row)).toEqual(
      sampleColorScheme(row.category, row.mappingName),
    );
    expect(scheduleFullRedraw).toHaveBeenCalledOnce();
    track.settings[0].update('function');
    expect(attrs.getKey!()).toBe(functionKey);
    expect(attrs.colorizer!(row)).toEqual(functionColor);
    expect(scheduleFullRedraw).toHaveBeenCalledTimes(2);
  });

  test('flamecharts share a bulk-edit descriptor but keep independent settings', () => {
    vi.spyOn(SliceTrack, 'create').mockReturnValue(
      {} as ReturnType<typeof SliceTrack.create>,
    );
    const trace = {raf: {scheduleFullRedraw: vi.fn()}} as unknown as Trace;
    const first = new CallstackTrack(trace, 'first', {
      source: 'linux.perf',
      utid: 1,
      upid: 1,
    });
    const second = new CallstackTrack(trace, 'second', {
      source: 'linux.perf',
      utid: 2,
      upid: 1,
    });
    expect(first.settings[0].descriptor).toBe(second.settings[0].descriptor);
    first.settings[0].update('mapping');
    expect(second.settings[0].value).toBe('function');
  });
});
