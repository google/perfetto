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

import {Optional} from '../base/utils';
import {Engine} from '../trace_processor/engine';
import {LegacySelection} from '../public/selection';
import {LegacySelectionDetails} from './selection_manager';
import {getSched, getSchedWakeupInfo} from '../trace_processor/sql_utils/sched';
import {
  asSchedSqlId,
  asSliceSqlId,
} from '../trace_processor/sql_utils/core_types';
import {getThreadState} from '../trace_processor/sql_utils/thread_state';
import {getSlice} from '../trace_processor/sql_utils/slice';

// This class queries the TP for the details on a specific slice that has
// been clicked.
export class SelectionResolver {
  constructor(private engine: Engine) {}

  async resolveSelection(
    selection: LegacySelection,
  ): Promise<Optional<LegacySelectionDetails>> {
    if (selection.kind === 'SCHED_SLICE') {
      const sched = await getSched(this.engine, asSchedSqlId(selection.id));
      if (sched === undefined) {
        return undefined;
      }
      const wakeup = await getSchedWakeupInfo(this.engine, sched);
      return {
        ts: sched.ts,
        dur: sched.dur,
        wakeupTs: wakeup?.wakeupTs,
        wakerCpu: wakeup?.wakerCpu,
      };
    } else if (selection.kind === 'THREAD_STATE') {
      const threadState = await getThreadState(this.engine, selection.id);
      if (threadState === undefined) {
        return undefined;
      }
      return {
        ts: threadState.ts,
        dur: threadState.dur,
      };
    } else if (selection.kind === 'SLICE') {
      const slice = await getSlice(this.engine, asSliceSqlId(selection.id));
      if (slice === undefined) {
        return undefined;
      }
      return {
        ts: slice.ts,
        dur: slice.dur,
      };
    }
    return undefined;
  }
}
