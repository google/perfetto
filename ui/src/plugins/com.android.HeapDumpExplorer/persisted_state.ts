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

import {z} from 'zod';
import {TREE_EXPLORER_STATE_SCHEMA} from '../../widgets/tree_explorer';

// Heap Dump Explorer state persisted in permalinks. Per-dump state is keyed
// by dumpKey() (see nav.ts).

const HDE_STATE_SCHEMA = z
  .object({
    version: z.literal(2),
    // The subpage last rendered (e.g. '/<dump>/objects/Foo'), including the
    // leading '/', so a permalink reopens on the same page.
    subpage: z.string().optional(),
    // Filter / pivot / view state of the main Flamegraph tab, per dump.
    flamegraphPanelStates: z
      .record(z.string(), TREE_EXPLORER_STATE_SCHEMA)
      .optional(),
    // Filter / pivot / view state of the Callstack tab, per dump.
    callstackPanelStates: z
      .record(z.string(), TREE_EXPLORER_STATE_SCHEMA)
      .optional(),
  })
  .readonly();

export type HdeState = z.infer<typeof HDE_STATE_SCHEMA>;

// An unparseable or older permalink falls back to empty state rather than
// throwing.
export function migrateHdeState(init: unknown): HdeState {
  return HDE_STATE_SCHEMA.safeParse(init).data ?? {version: 2};
}
