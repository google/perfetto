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

import {searchSegment} from '../base/binary_search';
import {Actions} from '../common/actions';
import {toNs} from '../common/time';

import {globals} from './globals';
import {scrollToTrackAndTs} from './scroll_helper';

export function executeSearch(reverse = false) {
  const state = globals.frontendLocalState;
  const index = state.searchIndex;
  const startNs = toNs(globals.frontendLocalState.visibleWindowTime.start);
  const endNs = toNs(globals.frontendLocalState.visibleWindowTime.end);
  const currentTs = globals.currentSearchResults.tsStarts[index];

  // If this is a new search or the currentTs is not in the viewport,
  // select the first/last item in the viewport.
  if (index === -1 || currentTs < startNs || currentTs > endNs) {
    if (reverse) {
      const [smaller,] =
        searchSegment(globals.currentSearchResults.tsStarts, endNs);
      globals.frontendLocalState.setSearchIndex(smaller);
    } else {
      const [, larger] =
          searchSegment(globals.currentSearchResults.tsStarts, startNs);
      globals.frontendLocalState.setSearchIndex(larger);
    }
    // If there is no result in the current viewport, move it.
    const currentTs = globals.currentSearchResults.tsStarts[state.searchIndex];
    if (currentTs < startNs || currentTs > endNs) {
      moveViewportToCurrentSearch();
    }
  } else {
    // If the currentTs is in the viewport, increment the index and move the
    // viewport if necessary.
    if (reverse) {
      globals.frontendLocalState.setSearchIndex(Math.max(index - 1, 0));
    } else {
      globals.frontendLocalState.setSearchIndex(Math.min(
          index + 1, globals.currentSearchResults.sliceIds.length - 1));
    }
    moveViewportToCurrentSearch();
  }
  selectCurrentSearchResult();
}

function moveViewportToCurrentSearch() {
  const currentTs = globals.currentSearchResults
                        .tsStarts[globals.frontendLocalState.searchIndex];
  const trackId = globals.currentSearchResults
                      .trackIds[globals.frontendLocalState.searchIndex];
  scrollToTrackAndTs(trackId, currentTs);
}

function selectCurrentSearchResult() {
  const state = globals.frontendLocalState;
  const index = state.searchIndex;
  const refType = globals.currentSearchResults.refTypes[index];
  const currentId = globals.currentSearchResults.sliceIds[index];

  if (currentId === undefined) return;

  if (refType === 'cpu') {
    globals.dispatch(Actions.selectSlice({
      id: currentId,
    }));
  } else {
    globals.dispatch(Actions.selectChromeSlice({
      id: currentId,
    }));
  }
}
