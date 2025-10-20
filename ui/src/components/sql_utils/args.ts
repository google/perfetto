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

import {Engine} from '../../trace_processor/engine';
import {
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {ArgSetId, ArgsId, asArgId} from './core_types';

export interface Arg {
  id: ArgsId;
  flatKey: string;
  key: string;
  displayValue: string;
}

export async function getArgs(
  engine: Engine,
  argSetId: ArgSetId,
): Promise<Arg[]> {
  const query = await engine.query(`
    SELECT
      id,
      flat_key as flatKey,
      key,
      int_value as intValue,
      string_value as stringValue,
      real_value as realValue,
      value_type as valueType,
      display_value as displayValue
    FROM args
    WHERE arg_set_id = ${argSetId}
    ORDER BY id`);
  const it = query.iter({
    id: NUM,
    flatKey: STR,
    key: STR,
    intValue: LONG_NULL,
    stringValue: STR_NULL,
    realValue: NUM_NULL,
    valueType: STR,
    displayValue: STR_NULL,
  });

  const result: Arg[] = [];
  for (; it.valid(); it.next()) {
    result.push({
      id: asArgId(it.id),
      flatKey: it.flatKey,
      key: it.key,
      displayValue: it.displayValue ?? 'NULL',
    });
  }
  return result;
}
