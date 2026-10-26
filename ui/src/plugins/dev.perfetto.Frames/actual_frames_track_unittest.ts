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
import {createActualFramesTrack} from './actual_frames_track';

describe('ActualFramesTrack Tooltips Configuration & Test Suite', () => {
  const fakeTrace = {engine: {}} as unknown as Trace;

  test('ActualFramesTrack scopes the dataset to a process', () => {
    const track = createActualFramesTrack(
      fakeTrace,
      '/process_1/actual_frames',
      2,
      1,
      false,
    );
    expect(track.rootTableName).toBe('slice');
    expect(track.getDataset().src).toContain('upid = 1');
    expect(track.getDataset().src).not.toContain('layer_name');
    // A upid dataset filter would map the events of both the expected and the
    // actual timeline of a process to the same track.
    expect(track.getDataset().filter).toBeUndefined();
  });

  test('ActualFramesTrack scopes the dataset to a layer of a process', () => {
    const track = createActualFramesTrack(
      fakeTrace,
      '/process_1/actual_frames/MyLayer',
      2,
      7,
      false,
      "My'Layer",
    );
    expect(track.rootTableName).toBeUndefined();
    // Layer names are escaped, hence the doubled up quote.
    expect(track.getDataset().src).toContain("layer_name = 'My''Layer'");
    expect(track.getDataset().src).toContain('upid = 7');
    expect(track.getDataset().filter).toBeUndefined();
  });
});
