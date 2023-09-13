// Copyright (C) 2021 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use size file except in compliance with the License.
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

import {duration, time} from '../base/time';
import {DurationWidget} from '../widgets/duration';

import {globals, SliceDetails} from './globals';

// To display process or thread, we want to concatenate their name with ID, but
// either can be undefined and all the cases need to be considered carefully to
// avoid `undefined undefined` showing up in the UI. This function does such
// concatenation.
//
// Result can be undefined if both name and process are, in this case result is
// not going to be displayed in the UI.
function getDisplayName(name: string|undefined, id: number|undefined): string|
    undefined {
  if (name === undefined) {
    return id === undefined ? undefined : `${id}`;
  } else {
    return id === undefined ? name : `${name} ${id}`;
  }
}

export abstract class SlicePanel implements m.ClassComponent {
  protected computeDuration(ts: time, dur: duration): m.Children {
    if (dur === -1n) {
      const minDuration = globals.state.traceTime.end - ts;
      return [m(DurationWidget, {dur: minDuration}), ' (Did not end)'];
    } else {
      return m(DurationWidget, {dur});
    }
  }

  protected getProcessThreadDetails(sliceInfo: SliceDetails) {
    return new Map<string, string|undefined>([
      ['Thread', getDisplayName(sliceInfo.threadName, sliceInfo.tid)],
      ['Process', getDisplayName(sliceInfo.processName, sliceInfo.pid)],
      ['User ID', sliceInfo.uid ? String(sliceInfo.uid) : undefined],
      ['Package name', sliceInfo.packageName],
      [
        'Version code',
        sliceInfo.versionCode ? String(sliceInfo.versionCode) : undefined,
      ],
    ]);
  }

  abstract view(vnode: m.Vnode): void|m.Children;
}
