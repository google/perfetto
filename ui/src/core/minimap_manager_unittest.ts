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

import {Time} from '../base/time';
import type {MinimapRow} from '../public/minimap';
import {MinimapManagerImpl} from './minimap_manager';

// Providers may update the rows in place, see MinimapContentProvider.
test('keeps the rows that the provider returned', async () => {
  const rows: MinimapRow[] = [];
  const minimap = new MinimapManagerImpl();
  minimap.registerContentProvider({
    priority: 1,
    getData: () => Promise.resolve(rows),
  });

  await minimap.load(Time.ZERO, Time.fromSeconds(10));
  expect(minimap.getLoad()).toBe(rows);
});
