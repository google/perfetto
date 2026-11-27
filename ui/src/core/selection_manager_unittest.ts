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

import type {
  AreaSelectionTab,
  ContentWithLoadingFlag,
  TrackEventSelectionTab,
} from '../public/selection';
import {createFakeTraceImpl} from './fake_trace_impl';
import {getOwner, setOwner} from './ownership';

const PLUGIN = 'com.example.Owner';
const CONTENT: ContentWithLoadingFlag = {isLoading: false, content: 'x'};

describe('SelectionManagerImpl ownership', () => {
  test('area selection tab adapter carries the owner', () => {
    const {selection} = createFakeTraceImpl();
    const tab: AreaSelectionTab = {
      id: 'area',
      name: 'Area',
      render: () => CONTENT,
    };
    setOwner(tab, PLUGIN);
    selection.registerAreaSelectionTab(tab);
    const stored = selection.selectionTabs.at(-1)!;
    expect(stored).not.toBe(tab);
    expect(getOwner(stored)).toBe(PLUGIN);
  });

  test('track event selection tab adapter carries the owner', () => {
    const {selection} = createFakeTraceImpl();
    const tab: TrackEventSelectionTab = {
      id: 'track_event',
      name: 'Track event',
      render: () => CONTENT,
    };
    setOwner(tab, PLUGIN);
    selection.registerTrackEventSelectionTab(tab);
    const stored = selection.selectionTabs.at(-1)!;
    expect(stored).not.toBe(tab);
    expect(getOwner(stored)).toBe(PLUGIN);
  });

  test('adapter leaves an unowned tab unowned', () => {
    const {selection} = createFakeTraceImpl();
    selection.registerAreaSelectionTab({
      id: 'area',
      name: 'Area',
      render: () => CONTENT,
    });
    expect(getOwner(selection.selectionTabs.at(-1)!)).toBeUndefined();
  });
});
