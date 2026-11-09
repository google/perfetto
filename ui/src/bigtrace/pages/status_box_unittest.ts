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

import {afterEach, expect, test, vi} from 'vitest';
import m from 'mithril';
import {queryStore} from '../query/query_store';
import {QueryTabsState} from './query_tabs_state';
import {renderStatusBox} from './status_box';

afterEach(() => {
  vi.useRealTimers();
  vi.restoreAllMocks();
});

test('the running time redraws every second until the run ends', () => {
  vi.useFakeTimers();
  const redraw = vi.spyOn(m, 'redraw').mockImplementation(() => {});
  const tab = new QueryTabsState().tabs[0]!;
  tab.queryUuid = 'uuid-ticking';
  tab.isLoading = true;
  tab.execution = queryStore.getOrCreate('uuid-ticking', {
    status: 'IN_PROGRESS',
    startTime: Date.now(),
  });
  const root = document.createElement('div');

  m.render(root, renderStatusBox(tab));
  vi.advanceTimersByTime(2000);
  expect(redraw).toHaveBeenCalledTimes(2);

  tab.execution.status = 'SUCCESS';
  tab.execution.endTime = Date.now();
  m.render(root, renderStatusBox(tab));
  vi.advanceTimersByTime(2000);
  expect(redraw).toHaveBeenCalledTimes(2);
});
