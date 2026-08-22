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

import type {TracePreset} from './bigtrace_query_client';

export const queryState = {
  initialQuery: undefined as string | undefined,
  // Set by a home-page preset card; consumed once by QueryPage to seed a tab.
  initialPreset: undefined as TracePreset | undefined,
  // Set by the settings-page "Query" button; consumed once by QueryPage to open
  // a fresh tab seeded from the current /settings globals (no SQL).
  seedTabFromSettings: false,
};
