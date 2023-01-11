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

function setToPrevious(current: number) {
  let index = current - 1;
  if (index < 0) {
    index = globals.currentSearchResults.totalResults - 1;
  }
  globals.dispatch(Actions.setSearchIndex({index}));
}

function setToNext(current: number) {
  const index =
      (current + 1) % globals.currentSearchResults.totalResults;
  globals.dispatch(Actions.setSearchIndex({index}));
}

export function executeSearch(reverse = false) {
  const index = globals.state.searchIndex;
  const startNs = toNs(globals.frontendLocalState.visibleWindowTime.start);
  const endNs = toNs(globals.frontendLocalState.visibleWindowTime.end);
  const currentTs = globals.currentSearchResults.tsStarts[index];

  // If the value of |globals.currentSearchResults.totalResults| is 0,
  // it means that the query is in progress or no results are found.
  if (globals.currentSearchResults.totalResults === 0) {
    return;
  }

  // If this is a new search or the currentTs is not in the viewport,
  // select the first/last item in the viewport.
  if (index === -1 || currentTs < startNs || currentTs > endNs) {
    if (reverse) {
      const [smaller] =
          searchSegment(globals.currentSearchResults.tsStarts, endNs);
      // If there is no item in the viewport just go to the previous.
      if (smaller === -1) {
        setToPrevious(index);
      } else {
        globals.dispatch(Actions.setSearchIndex({index: smaller}));
      }
    } else {
      const [, larger] =
          searchSegment(globals.currentSearchResults.tsStarts, startNs);
      // If there is no item in the viewport just go to the next.
      if (larger === -1) {
        setToNext(index);
      } else {
        globals.dispatch(Actions.setSearchIndex({index: larger}));
      }
    }
  } else {
    // If the currentTs is in the viewport, increment the index.
    if (reverse) {
      setToPrevious(index);
    } else {
      setToNext(index);
    }
  }
  selectCurrentSearchResult();
}

function selectCurrentSearchResult() {
  const searchIndex = globals.state.searchIndex;
  const source = globals.currentSearchResults.sources[searchIndex];
  const currentId = globals.currentSearchResults.sliceIds[searchIndex];
  const trackId = globals.currentSearchResults.trackIds[searchIndex];

  if (currentId === undefined) return;

  if (source === 'cpu') {
    globals.dispatch(
        Actions.selectSlice({id: currentId, trackId, scroll: true}));
  } else if (source === 'log') {
    globals.dispatch(Actions.selectLog({id: currentId, trackId, scroll: true}));
  } else {
    // Search results only include slices from the slice table for now.
    // When we include annotations we need to pass the correct table.
    globals.dispatch(Actions.selectChromeSlice(
        {id: currentId, trackId, table: 'slice', scroll: true}));
  }
}
