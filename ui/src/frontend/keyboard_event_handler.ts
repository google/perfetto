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
  if (down && 'v' === key) {
    globals.dispatch(Actions.toggleVideo({}));
  }
  if (down && 'p' === key) {
    globals.dispatch(Actions.toggleFlagPause({}));
  }
  if (down && 't' === key) {
    globals.dispatch(Actions.toggleScrubbing({}));
    if (globals.frontendLocalState.vidTimestamp < 0) {
      globals.frontendLocalState.setVidTimestamp(Number.MAX_SAFE_INTEGER);
    } else {
      globals.frontendLocalState.setVidTimestamp(Number.MIN_SAFE_INTEGER);
    }
  }
}

function selectSliceSpan() {
  const selection = globals.state.currentSelection;
  let startTs = -1;
  let endTs = -1;
  if (selection === null) return;

  if (selection.kind === 'SLICE' || selection.kind === 'CHROME_SLICE') {
    const slice = globals.sliceDetails;
    if (slice.ts && slice.dur) {
      startTs = slice.ts + globals.state.traceTime.startSec;
      endTs = startTs + slice.dur;
    }
  } else if (selection.kind === 'THREAD_STATE') {
    startTs = selection.ts;
    endTs = startTs + selection.dur;
  }

  if (startTs !== -1 && endTs !== -1) {
    globals.dispatch(Actions.selectTimeSpan({startTs, endTs}));
  }
}
