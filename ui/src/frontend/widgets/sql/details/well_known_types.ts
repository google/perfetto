// Copyright (C) 2023 The Android Open Source Project
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
  ProcessInfo,
  getProcessInfo,
} from '../../../../trace_processor/sql_utils/process';
import {
  SliceDetails,
  getSlice,
} from '../../../../trace_processor/sql_utils/slice';
import {
  asSliceSqlId,
  asUpid,
  asUtid,
} from '../../../../trace_processor/sql_utils/core_types';
import {
  ThreadInfo,
  getThreadInfo,
} from '../../../../trace_processor/sql_utils/thread';
import {renderProcessRef} from '../../process';
import {sliceRef} from '../../slice';
import {renderThreadRef} from '../../thread';
import {createSqlIdRefRenderer, SqlIdRefRenderer} from './details';

export const wellKnownTypes: {[key: string]: SqlIdRefRenderer} = {
  process: createSqlIdRefRenderer<ProcessInfo>(
    async (engine, id) => await getProcessInfo(engine, asUpid(Number(id))),
    (data: ProcessInfo) => ({
      value: renderProcessRef(data),
    }),
  ),
  thread: createSqlIdRefRenderer<ThreadInfo>(
    async (engine, id) => await getThreadInfo(engine, asUtid(Number(id))),
    (data: ThreadInfo) => ({
      value: renderThreadRef(data),
    }),
  ),
  slice: createSqlIdRefRenderer<{slice: SliceDetails | undefined; id: bigint}>(
    async (engine, id) => {
      return {
        id,
        slice: await getSlice(engine, asSliceSqlId(Number(id))),
      };
    },
    ({id, slice}) => ({
      value: slice !== undefined ? sliceRef(slice) : `Unknown slice ${id}`,
    }),
  ),
};
