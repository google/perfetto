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
import {FLAMEGRAPH_STATE_SCHEMA} from '../../widgets/flamegraph';

// Schema for the slice of Heap Dump Explorer state that survives in a shared
// permalink. The session writes this on every state change via mountStore;
// the core serializes it into the permalink and restores it before the plugin
// loads (see core/state_serialization.ts).
//
// Timestamps are heap dump `ts` values (bigint) and are stored as decimal
// strings because JSON has no bigint.

const DUMP_REF_SCHEMA = z.object({
  upid: z.number(),
  ts: z.string(),
});

// A flamegraph tab always belongs to the active dump (tabs reset on dump
// switch), so the dump is taken from activeDump rather than stored per tab.
const FLAMEGRAPH_TAB_SCHEMA = z.object({
  pathHashes: z.string(),
  isDominator: z.boolean(),
});

const INSTANCE_TAB_SCHEMA = z.object({
  objId: z.number(),
  label: z.string(),
});

export const HDE_STATE_SCHEMA = z
  .object({
    // The selected heap dump; identifies which dump the rest of the state
    // belongs to. Restore is skipped if it no longer matches a loaded dump.
    activeDump: DUMP_REF_SCHEMA.optional(),
    // The active navigation, as a stateToSubpage subpage string.
    nav: z.string().optional(),
    // Open "Flamegraph objects" drill-down tabs. The active one is not stored;
    // it is re-derived from nav (which encodes the tab's pathHashes) on restore.
    flamegraphTabs: z.array(FLAMEGRAPH_TAB_SCHEMA).optional(),
    // Open object/instance inspector tabs. The active one is not stored; it is
    // re-derived from nav (which encodes the object id) on restore.
    instanceTabs: z.array(INSTANCE_TAB_SCHEMA).optional(),
    // Filter / pivot / view state of the main Flamegraph tab.
    flamegraphPanelState: FLAMEGRAPH_STATE_SCHEMA.optional(),
    // Filter / pivot / view state of the Callstack tab.
    callstackPanelState: FLAMEGRAPH_STATE_SCHEMA.optional(),
  })
  .readonly();

export type HdeState = z.infer<typeof HDE_STATE_SCHEMA>;

// An unparseable or older permalink falls back to empty state rather than
// throwing.
export function migrateHdeState(init: unknown): HdeState {
  return HDE_STATE_SCHEMA.safeParse(init).data ?? {};
}
