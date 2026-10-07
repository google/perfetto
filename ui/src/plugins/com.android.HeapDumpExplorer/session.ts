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

import type {Store} from '../../base/store';
import type {Trace} from '../../public/trace';
import type {TreeExplorerState} from '../../widgets/tree_explorer';
import {type DumpRef, type EphemeralHdeLink, dumpKey, makeHref} from './nav';
import type {HdeState} from './persisted_state';
import type * as queries from './queries';

// Trace-derived data plus per-dump panel state (persisted in the permalink
// store). What's showing lives in the URL (see makeHref).
export class HeapDumpExplorerSession {
  constructor(
    readonly trace: Trace,
    private readonly store: Store<HdeState>,
    readonly dumps: readonly queries.HeapDump[],
    // Whether the trace has HPROF field values (see traceHasFieldValues).
    readonly hasFieldValues: boolean,
  ) {}

  // The loaded dump with the given dumpKey(), if any.
  findDump(key: string): queries.HeapDump | undefined {
    return this.dumps.find((d) => dumpKey(d) === key);
  }

  flamegraphPanelState(dump: DumpRef): TreeExplorerState | undefined {
    return this.store.state.flamegraphPanelStates?.[dumpKey(dump)];
  }

  setFlamegraphPanelState(dump: DumpRef, state: TreeExplorerState): void {
    this.store.edit((s) => {
      s.flamegraphPanelStates = {
        ...s.flamegraphPanelStates,
        [dumpKey(dump)]: state,
      };
    });
  }

  callstackPanelState(dump: DumpRef): TreeExplorerState | undefined {
    return this.store.state.callstackPanelStates?.[dumpKey(dump)];
  }

  setCallstackPanelState(dump: DumpRef, state: TreeExplorerState): void {
    this.store.edit((s) => {
      s.callstackPanelStates = {
        ...s.callstackPanelStates,
        [dumpKey(dump)]: state,
      };
    });
  }

  // The dump's pinned tabs, in tab order.
  pinnedTabs(dump: DumpRef): readonly EphemeralHdeLink[] {
    return this.store.state.pinnedTabs?.[dumpKey(dump)] ?? [];
  }

  isPinned(dump: DumpRef, link: EphemeralHdeLink): boolean {
    const href = makeHref(dump, link);
    return this.pinnedTabs(dump).some((t) => makeHref(dump, t) === href);
  }

  // Appends `link` to the dump's pinned tabs (no-op if already pinned).
  pinTab(dump: DumpRef, link: EphemeralHdeLink): void {
    if (this.isPinned(dump, link)) return;
    const tabs = [...this.pinnedTabs(dump), link];
    this.store.edit((s) => {
      s.pinnedTabs = {...s.pinnedTabs, [dumpKey(dump)]: tabs};
    });
  }

  unpinTab(dump: DumpRef, link: EphemeralHdeLink): void {
    const href = makeHref(dump, link);
    const tabs = this.pinnedTabs(dump).filter(
      (t) => makeHref(dump, t) !== href,
    );
    this.store.edit((s) => {
      s.pinnedTabs = {...s.pinnedTabs, [dumpKey(dump)]: tabs};
    });
  }
}
