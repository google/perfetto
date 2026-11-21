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

import {
  type NavState,
  memoryLocationToSubpage,
} from '../../com.android.HeapDumpExplorer/nav_state';

export const MEMORY_EXPLORER_ROUTE = '/memory';

// Route of the Memory Explorer page, e.g. `/memory/12/summary`. Without a
// upid the page picks the most interesting process.
export function memoryExplorerRoute(upid?: number, nav?: NavState): string {
  if (upid === undefined) return MEMORY_EXPLORER_ROUTE;
  const sub = memoryLocationToSubpage({
    upid,
    nav: nav ?? {view: 'summary', params: {}},
  });
  return `${MEMORY_EXPLORER_ROUTE}/${sub}`;
}

// memoryExplorerRoute() as an href for links and trace.navigate().
export function memoryExplorerHref(upid?: number, nav?: NavState): string {
  return `#!${memoryExplorerRoute(upid, nav)}`;
}
