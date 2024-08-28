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

import {exists} from '../base/utils';
import {Actions} from '../common/actions';
import {getLegacySelection} from '../common/state';

import {Flow, globals} from './globals';
import {focusHorizontalRange, verticalScrollToTrack} from './scroll_helper';

type Direction = 'Forward' | 'Backward';

// Search |boundFlows| for |flowId| and return the id following it.
// Returns the first flow id if nothing was found or |flowId| was the last flow
// in |boundFlows|, and -1 if |boundFlows| is empty
function findAnotherFlowExcept(boundFlows: Flow[], flowId: number): number {
  let selectedFlowFound = false;

  if (boundFlows.length === 0) {
    return -1;
  }

  for (const flow of boundFlows) {
    if (selectedFlowFound) {
      return flow.id;
    }

    if (flow.id === flowId) {
      selectedFlowFound = true;
    }
  }
  return boundFlows[0].id;
}

// Change focus to the next flow event (matching the direction)
export function focusOtherFlow(direction: Direction) {
  const currentSelection = getLegacySelection(globals.state);
  if (!currentSelection || currentSelection.kind !== 'SLICE') {
    return;
  }
  const sliceId = currentSelection.id;
  if (sliceId === -1) {
    return;
  }

  const boundFlows = globals.connectedFlows.filter(
    (flow) =>
      (flow.begin.sliceId === sliceId && direction === 'Forward') ||
      (flow.end.sliceId === sliceId && direction === 'Backward'),
  );

  if (direction === 'Backward') {
    const nextFlowId = findAnotherFlowExcept(
      boundFlows,
      globals.state.focusedFlowIdLeft,
    );
    globals.dispatch(Actions.setHighlightedFlowLeftId({flowId: nextFlowId}));
  } else {
    const nextFlowId = findAnotherFlowExcept(
      boundFlows,
      globals.state.focusedFlowIdRight,
    );
    globals.dispatch(Actions.setHighlightedFlowRightId({flowId: nextFlowId}));
  }
}

// Select the slice connected to the flow in focus
export function moveByFocusedFlow(direction: Direction): void {
  const currentSelection = getLegacySelection(globals.state);
  if (!currentSelection || currentSelection.kind !== 'SLICE') {
    return;
  }

  const sliceId = currentSelection.id;
  const flowId =
    direction === 'Backward'
      ? globals.state.focusedFlowIdLeft
      : globals.state.focusedFlowIdRight;

  if (sliceId === -1 || flowId === -1) {
    return;
  }

  // Find flow that is in focus and select corresponding slice
  for (const flow of globals.connectedFlows) {
    if (flow.id === flowId) {
      const flowPoint = direction === 'Backward' ? flow.begin : flow.end;
      const trackKeyByTrackId = globals.trackManager.trackKeyByTrackId;
      const trackKey = trackKeyByTrackId.get(flowPoint.trackId);
      if (trackKey) {
        globals.setLegacySelection(
          {
            kind: 'SLICE',
            id: flowPoint.sliceId,
            trackKey,
            table: 'slice',
          },
          {
            clearSearch: true,
            pendingScrollId: flowPoint.sliceId,
            switchToCurrentSelectionTab: true,
          },
        );
      }
    }
  }
}

export async function findCurrentSelection() {
  const selection = getLegacySelection(globals.state);
  if (selection === null) return;

  const range = await globals.findTimeRangeOfSelection();
  if (exists(range)) {
    focusHorizontalRange(range.start, range.end);
  }

  if (selection.trackKey) {
    verticalScrollToTrack(selection.trackKey, true);
  }
}
