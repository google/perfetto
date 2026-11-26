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

import {
  groupInputRows,
  type InputVideoRow,
  shortChannelName,
} from './video_frame_input_section';

function row(overrides: Partial<InputVideoRow>): InputVideoRow {
  return {
    motionEventId: 1,
    eventTs: 100n,
    actionMasked: 0,
    processName: null,
    channel: null,
    visibleFrameId: 10,
    responseFrameId: null,
    isResponseExact: null,
    inputToPhotonDur: null,
    dispatchSliceId: null,
    receiveSliceId: null,
    ...overrides,
  };
}

describe('groupInputRows', () => {
  test('one event per motion event, one receiver per channel', () => {
    const events = groupInputRows([
      row({
        processName: 'com.example.app',
        responseFrameId: 12,
        isResponseExact: 1,
        inputToPhotonDur: 30n,
        dispatchSliceId: 5,
        receiveSliceId: 6,
      }),
      row({processName: 'com.android.systemui'}),
      row({motionEventId: 2, eventTs: 200n, actionMasked: 1}),
    ]);
    expect(events).toHaveLength(2);
    const [down, up] = events;
    expect(down!.action).toBe('DOWN');
    expect(down!.visibleFrameId).toBe(10);
    expect(down!.receivers).toEqual([
      {
        process: 'com.example.app',
        channel: undefined,
        dispatchSliceId: 5,
        receiveSliceId: 6,
        responseFrameId: 12,
        isResponseExact: true,
        inputToPhoton: 30n,
      },
      {
        process: 'com.android.systemui',
        channel: undefined,
        dispatchSliceId: undefined,
        receiveSliceId: undefined,
        responseFrameId: undefined,
        isResponseExact: false,
        inputToPhoton: undefined,
      },
    ]);
    // Without atrace: a single receiver with no window nor response.
    expect(up!.action).toBe('UP');
    expect(up!.receivers).toHaveLength(1);
    expect(up!.receivers[0]!.process).toBeUndefined();
    expect(up!.receivers[0]!.responseFrameId).toBeUndefined();
  });

  test('keeps each channel its own response', () => {
    const [event] = groupInputRows([
      row({
        processName: 'com.example.app',
        responseFrameId: 12,
        isResponseExact: 1,
        inputToPhotonDur: 30n,
      }),
      row({
        processName: 'com.android.systemui',
        responseFrameId: 14,
        isResponseExact: 0,
        inputToPhotonDur: 50n,
      }),
    ]);
    expect(
      event!.receivers.map((r) => [
        r.process,
        r.responseFrameId,
        r.isResponseExact,
        r.inputToPhoton,
      ]),
    ).toEqual([
      ['com.example.app', 12, true, 30n],
      ['com.android.systemui', 14, false, 50n],
    ]);
  });

  test('names unknown actions by number', () => {
    expect(groupInputRows([row({actionMasked: 9})])[0]!.action).toBe('9');
  });
});

describe('shortChannelName', () => {
  const launcher = 'com.google.android.apps.nexuslauncher';
  test('activity and class names lose their package', () => {
    expect(
      shortChannelName(
        `${launcher}/${launcher}.NexusLauncherActivity`,
        launcher,
      ),
    ).toBe('NexusLauncherActivity');
    expect(
      shortChannelName(
        'com.android.systemui.wallpapers.ImageWallpaper',
        'com.android.systemui',
      ),
    ).toBe('ImageWallpaper');
  });

  test('keeps plain and monitor names', () => {
    expect(shortChannelName('Taskbar', launcher)).toBe('Taskbar');
    expect(shortChannelName('[Gesture Monitor] swipe-up', launcher)).toBe(
      '[Gesture Monitor] swipe-up',
    );
  });

  test('omits a name that repeats the process', () => {
    expect(shortChannelName(launcher, launcher)).toBeUndefined();
  });
});
