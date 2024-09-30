// Copyright (C) 2024 The Android Open Source Project
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

import {Trace} from '../public/trace';
import {type AddSqlTableTabParams} from './sql_table_tab';

type AddSqlTableTabFunction = (
  trace: Trace,
  config: AddSqlTableTabParams,
) => void;

// TODO(primiano): this injection is to break the circular dependency cycle that
// there is between DebugSliceTrack and SqlTableTab. The problem is:
// DebugSliceTrack has a DebugSliceDetailsTab which shows details about slices,
// which have a context menu, which allows to create a debug track from it.
// We should probably break this cycle "more properly" by having a registry for
// context menu items for slices.

let addSqlTableTabFunction: AddSqlTableTabFunction;

export function addSqlTableTab(
  trace: Trace,
  config: AddSqlTableTabParams,
): void {
  addSqlTableTabFunction(trace, config);
}

export function setAddSqlTableTabImplFunction(f: AddSqlTableTabFunction) {
  addSqlTableTabFunction = f;
}
