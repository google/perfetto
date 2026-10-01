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
import {type DumpRef, dumpKey} from './nav';
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
}
