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

import {Engine} from '../engine';
import {LONG_NULL, NUM, NUM_NULL, STR, STR_NULL} from '../query_result';
import {ArgSetId, ArgsId, asArgId} from './core_types';

export type ArgValue = bigint | string | number | boolean | null;
type ArgValueType =
  | 'int'
  | 'uint'
  | 'pointer'
  | 'string'
  | 'bool'
  | 'real'
  | 'null';

export interface Arg {
  id: ArgsId;
  type: string;
  flatKey: string;
  key: string;
  value: ArgValue;
  displayValue: string;
}

export async function getArgs(
  engine: Engine,
  argSetId: ArgSetId,
): Promise<Arg[]> {
  const query = await engine.query(`
    SELECT
      id,
      type,
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
    type: STR,
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
    const value = parseValue(it.valueType as ArgValueType, it);
    result.push({
      id: asArgId(it.id),
      type: it.type,
      flatKey: it.flatKey,
      key: it.key,
      value,
      displayValue: it.displayValue ?? 'NULL',
    });
  }

  return result;
}

function parseValue(
  valueType: ArgValueType,
  value: {
    intValue: bigint | null;
    stringValue: string | null;
    realValue: number | null;
  },
): ArgValue {
  switch (valueType) {
    case 'int':
    case 'uint':
      return value.intValue;
    case 'pointer':
      return value.intValue === null
        ? null
        : `0x${value.intValue.toString(16)}`;
    case 'string':
      return value.stringValue;
    case 'bool':
      // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
      return !!value.intValue;
    case 'real':
      return value.realValue;
    case 'null':
      return null;
    default:
      const x: number = valueType;
      throw new Error(`Unable to process arg of type ${x}`);
  }
}
