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

import {assertUnreachable} from '../base/logging';
import {ScrollHelper} from '../core/scroll_helper';
import {SelectionManagerImpl} from '../core/selection_manager';
import {SearchResult} from '../public/search';

export function selectCurrentSearchResult(
  step: SearchResult,
  selectionManager: SelectionManagerImpl,
  scrollHelper: ScrollHelper,
) {
  const {source, eventId, trackUri} = step;
  if (eventId === undefined) {
    return;
  }
  switch (source) {
    case 'track':
      scrollHelper.scrollTo({track: {uri: trackUri, expandGroup: true}});
      break;
    case 'cpu':
      selectionManager.setLegacy(
        {
          kind: 'SCHED_SLICE',
          id: eventId,
          trackUri,
        },
        {
          clearSearch: false,
          pendingScrollId: eventId,
          switchToCurrentSelectionTab: true,
        },
      );
      break;
    case 'log':
      selectionManager.setLegacy(
        {
          kind: 'LOG',
          id: eventId,
          trackUri,
        },
        {
          clearSearch: false,
          switchToCurrentSelectionTab: true,
        },
      );
      break;
    case 'slice':
      // Search results only include slices from the slice table for now.
      // When we include annotations we need to pass the correct table.
      selectionManager.setLegacy(
        {
          kind: 'SLICE',
          id: eventId,
          trackUri,
          table: 'slice',
        },
        {
          clearSearch: false,
          pendingScrollId: eventId,
          switchToCurrentSelectionTab: true,
        },
      );
      break;
    default:
      assertUnreachable(source);
  }
}
