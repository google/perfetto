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

import type m from 'mithril';
import {vi} from 'vitest';
import {Time} from '../../base/time';
import type {Trace} from '../../public/trace';
import {FlamechartFrameDetailsPanel} from './frame_details_panel';

vi.mock('../../components/time_utils', () => ({formatDuration: () => '20 ns'}));

describe('FlamechartFrameDetailsPanel', () => {
  test.each([
    {dur: -1n, end: 100n, label: 'Incomplete (sampled)'},
    {dur: 20n, end: 30n, label: '20 ns (sampled)'},
  ])('selects the displayed range for dur=$dur', ({dur, end, label}) => {
    const selectArea = vi.fn();
    const trace = {
      traceInfo: {end: Time.fromRaw(100n)},
      selection: {selectArea},
    } as unknown as Trace;
    const panel = new FlamechartFrameDetailsPanel(trace, {
      frameId: 1,
      name: 'work',
      ts: Time.fromRaw(10n),
      dur,
      category: 0,
      sampleCount: 2,
      trackUri: 'callstacks',
    });
    const result = panel.render() as m.Vnode<{
      description: string;
      buttons: m.Vnode<{onclick: () => void}>;
    }>;
    expect(result.attrs.description).toContain(label);
    result.attrs.buttons.attrs.onclick();
    expect(selectArea).toHaveBeenCalledWith({
      start: Time.fromRaw(10n),
      end: Time.fromRaw(end),
      trackUris: ['callstacks'],
    });
  });
});
