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

import {Actions} from '../common/actions';
import {Area} from '../common/state';

import {Flow, globals} from './globals';
import {toggleHelp} from './help_modal';
import {
  focusHorizontalRange,
  verticalScrollToTrack,
} from './scroll_helper';
import {executeSearch} from './search_handler';

const INSTANT_FOCUS_DURATION_S = 1 / 1e9;  // 1 ns.
type Direction = 'Forward'|'Backward';

// Handles all key events than are not handled by the
// pan and zoom handler. Returns true if the event was handled.
export function handleKey(e: KeyboardEvent, down: boolean): boolean {
  const key = e.key.toLowerCase();
  const selection = globals.state.currentSelection;
  const noModifiers = !(e.ctrlKey || e.metaKey || e.altKey || e.shiftKey);
  const ctrlOrMeta = (e.ctrlKey || e.metaKey) && !(e.altKey || e.shiftKey);
  // No other modifiers other than possibly Shift.
  const maybeShift = !(e.ctrlKey || e.metaKey || e.altKey);

  if (down && 'm' === key && maybeShift) {
    if (selection && selection.kind === 'AREA') {
      globals.dispatch(Actions.toggleMarkCurrentArea({persistent: e.shiftKey}));
    } else if (selection) {
      lockSliceSpan(e.shiftKey);
    }
    return true;
  }
  if (down && 'f' === key && noModifiers) {
    findCurrentSelection();
    return true;
  }
  if (down && 'a' === key && ctrlOrMeta) {
    let tracksToSelect: string[] = [];

    const selection = globals.state.currentSelection;
    if (selection !== null && selection.kind === 'AREA') {
      const area = globals.state.areas[selection.areaId];
      const coversEntireTimeRange =
          globals.state.traceTime.startSec === area.startSec &&
          globals.state.traceTime.endSec === area.endSec;
      if (!coversEntireTimeRange) {
        // If the current selection is an area which does not cover the entire
        // time range, preserve the list of selected tracks and expand the time
        // range.
        tracksToSelect = area.tracks;
      } else {
        // If the entire time range is already covered, update the selection to
        // cover all tracks.
        tracksToSelect = Object.keys(globals.state.tracks);
      }
    } else {
      // If the current selection is not an area, select all.
      tracksToSelect = Object.keys(globals.state.tracks);
    }
    globals.dispatch(Actions.selectArea({
      area: {
        startSec: globals.state.traceTime.startSec,
        endSec: globals.state.traceTime.endSec,
        tracks: tracksToSelect,
      },
    }));
    e.preventDefault();
    return true;
  }
  if (down && 'b' === key && ctrlOrMeta) {
    globals.dispatch(Actions.toggleSidebar({}));
    return true;
  }
  if (down && '?' === key && maybeShift) {
    toggleHelp();
    return true;
  }
  if (down && 'enter' === key && maybeShift) {
    e.preventDefault();
    executeSearch(e.shiftKey);
    return true;
  }
  if (down && 'escape' === key) {
    globals.frontendLocalState.deselectArea();
    globals.makeSelection(Actions.deselect({}));
    globals.dispatch(Actions.removeNote({id: '0'}));
    return true;
  }
  if (down && ']' === key && ctrlOrMeta) {
    focusOtherFlow('Forward');
    return true;
  }
  if (down && ']' === key && noModifiers) {
    moveByFocusedFlow('Forward');
    return true;
  }
  if (down && '[' === key && ctrlOrMeta) {
    focusOtherFlow('Backward');
    return true;
  }
  if (down && '[' === key && noModifiers) {
    moveByFocusedFlow('Backward');
    return true;
  }
  return false;
}

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
function focusOtherFlow(direction: Direction) {
  if (!globals.state.currentSelection ||
      globals.state.currentSelection.kind !== 'CHROME_SLICE') {
    return;
  }
  const sliceId = globals.state.currentSelection.id;
  if (sliceId === -1) {
    return;
  }

  const boundFlows = globals.connectedFlows.filter(
      (flow) => flow.begin.sliceId === sliceId && direction === 'Forward' ||
          flow.end.sliceId === sliceId && direction === 'Backward');

  if (direction === 'Backward') {
    const nextFlowId =
        findAnotherFlowExcept(boundFlows, globals.state.focusedFlowIdLeft);
    globals.dispatch(Actions.setHighlightedFlowLeftId({flowId: nextFlowId}));
  } else {
    const nextFlowId =
        findAnotherFlowExcept(boundFlows, globals.state.focusedFlowIdRight);
    globals.dispatch(Actions.setHighlightedFlowRightId({flowId: nextFlowId}));
  }
}

// Select the slice connected to the flow in focus
function moveByFocusedFlow(direction: Direction): void {
  if (!globals.state.currentSelection ||
      globals.state.currentSelection.kind !== 'CHROME_SLICE') {
    return;
  }

  const sliceId = globals.state.currentSelection.id;
  const flowId =
      (direction === 'Backward' ? globals.state.focusedFlowIdLeft :
                                  globals.state.focusedFlowIdRight);

  if (sliceId === -1 || flowId === -1) {
    return;
  }

  // Find flow that is in focus and select corresponding slice
  for (const flow of globals.connectedFlows) {
    if (flow.id === flowId) {
      const flowPoint = (direction === 'Backward' ? flow.begin : flow.end);
      const uiTrackId =
          globals.state.uiTrackIdByTraceTrackId[flowPoint.trackId];
      if (uiTrackId) {
        globals.makeSelection(Actions.selectChromeSlice({
          id: flowPoint.sliceId,
          trackId: uiTrackId,
          table: 'slice',
          scroll: true,
        }));
      }
    }
  }
}

function findTimeRangeOfSelection(): {startTs: number, endTs: number} {
  const selection = globals.state.currentSelection;
  let startTs = -1;
  let endTs = -1;
  if (selection === null) {
    return {startTs, endTs};
  } else if (selection.kind === 'SLICE' || selection.kind === 'CHROME_SLICE') {
    const slice = globals.sliceDetails;
    if (slice.ts && slice.dur !== undefined && slice.dur > 0) {
      startTs = slice.ts + globals.state.traceTime.startSec;
      endTs = startTs + slice.dur;
    } else if (slice.ts) {
      startTs = slice.ts + globals.state.traceTime.startSec;
      // This will handle either:
      // a)slice.dur === -1 -> unfinished slice
      // b)slice.dur === 0  -> instant event
      endTs = slice.dur === -1 ? globals.state.traceTime.endSec :
                                 startTs + INSTANT_FOCUS_DURATION_S;
    }
  } else if (selection.kind === 'THREAD_STATE') {
    const threadState = globals.threadStateDetails;
    if (threadState.ts && threadState.dur) {
      startTs = threadState.ts + globals.state.traceTime.startSec;
      endTs = startTs + threadState.dur;
    }
  } else if (selection.kind === 'COUNTER') {
    startTs = selection.leftTs;
    endTs = selection.rightTs;
  } else if (selection.kind === 'AREA') {
    const selectedArea = globals.state.areas[selection.areaId];
    if (selectedArea) {
      startTs = selectedArea.startSec;
      endTs = selectedArea.endSec;
    }
  } else if (selection.kind === 'NOTE') {
    const selectedNote = globals.state.notes[selection.id];
    // Notes can either be default or area notes. Area notes are handled
    // above in the AREA case.
    if (selectedNote && selectedNote.noteType === 'DEFAULT') {
      startTs = selectedNote.timestamp;
      endTs = selectedNote.timestamp + INSTANT_FOCUS_DURATION_S;
    }
  } else if (selection.kind === 'LOG') {
    // TODO(hjd): Make focus selection work for logs.
  }

  return {startTs, endTs};
}


function lockSliceSpan(persistent = false) {
  const range = findTimeRangeOfSelection();
  if (range.startTs !== -1 && range.endTs !== -1 &&
      globals.state.currentSelection !== null) {
    const tracks = globals.state.currentSelection.trackId ?
        [globals.state.currentSelection.trackId] :
        [];
    const area: Area = {startSec: range.startTs, endSec: range.endTs, tracks};
    globals.dispatch(Actions.markArea({area, persistent}));
  }
}

export function findCurrentSelection() {
  const selection = globals.state.currentSelection;
  if (selection === null) return;

  const range = findTimeRangeOfSelection();
  if (range.startTs !== -1 && range.endTs !== -1) {
    focusHorizontalRange(range.startTs, range.endTs);
  }

  if (selection.trackId) {
    verticalScrollToTrack(selection.trackId, true);
  }
}
