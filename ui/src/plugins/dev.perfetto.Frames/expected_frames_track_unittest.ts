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

import type {Trace} from '../../public/trace';
import {createExpectedFramesTrack} from './expected_frames_track';

describe('ExpectedFramesTrack', () => {
  const fakeTrace = {engine: {}} as unknown as Trace;

  test('reads the whole table when no layer is given', () => {
    const track = createExpectedFramesTrack(
      fakeTrace,
      '/process_1/expected_frames',
      2,
      [10, 11],
    );
    expect(track.getDataset().src).toBe('expected_frame_timeline_slice');
  });

  test('scopes the dataset to a layer of a process', () => {
    const track = createExpectedFramesTrack(
      fakeTrace,
      '/process_1/expected_frames/MyLayer',
      2,
      [10, 11],
      {name: "My'Layer", upid: 7},
    );
    // Layer names are escaped, hence the doubled up quote.
    expect(track.getDataset().src).toContain("layer_name = 'My''Layer'");
    expect(track.getDataset().src).toContain('upid = 7');
  });

  test('also picks up expected frames matched by surface frame token', () => {
    const track = createExpectedFramesTrack(
      fakeTrace,
      '/process_1/expected_frames/MyLayer',
      2,
      [10, 11],
      {name: 'MyLayer', upid: 7},
    );
    expect(track.getDataset().src).toContain(
      'act.surface_frame_token = exp.surface_frame_token',
    );
  });
});
