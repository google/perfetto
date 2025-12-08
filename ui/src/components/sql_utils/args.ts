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

import {parseJsonWithBigints} from '../../base/json_utils';
import {Engine} from '../../trace_processor/engine';
import {STR_NULL} from '../../trace_processor/query_result';
import {ArgSetId} from './core_types';

export type ArgValue = string | number | boolean | bigint | null;
export type Args = ArgValue | Args[] | ArgsDict;
export type ArgsDict = {[key: string]: Args};

export function parseArgs(args: string): ArgsDict {
  return parseJsonWithBigints(args) as ArgsDict;
}

export async function getArgs(
  engine: Engine,
  argSetId: ArgSetId,
): Promise<ArgsDict> {
  const query = await engine.query(`
    SELECT __intrinsic_arg_set_to_json(${argSetId}) as args_json
  `);
  const it = query.iter({
    args_json: STR_NULL,
  });

  if (!it.valid() || it.args_json === null) {
    return {};
  }

  const argsDict = parseJsonWithBigints(it.args_json);
  return argsDict;
}
