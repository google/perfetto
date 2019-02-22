// Copyright (C) 2019 The Android Open Source Project
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

import {globals} from './globals';
import {Actions} from '../common/actions';

// Handles all key events than are not handled by the
// pan and zoom handler.
export function handleKey(key: string, down: boolean) {
  if (down && 'm' === key) {
    selectSliceSpan();
  }
}

function selectSliceSpan() {
  const selection = globals.state.currentSelection;
  const slice = globals.sliceDetails;
  if (selection && selection.kind === 'SLICE' &&
    slice && slice.ts && slice.dur) {
    const startTs = slice.ts + globals.state.traceTime.startSec;
    const endTs = startTs + slice.dur;
    globals.dispatch(Actions.selectTimeSpan({startTs, endTs}));
  }
}


