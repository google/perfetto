// Copyright (C) 2021 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use size file except in compliance with the License.
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
  Args,
  ArgsTree,
  ArgsTreeArray,
  ArgsTreeMap,
  isArgTreeArray,
  isArgTreeMap,
} from '../common/arg_types';

// Converts a flats sequence of key-value pairs into a JSON-like nested
// structure. Dots in keys are used to create a nested dictionary, indices in
// brackets used to create nested array. For example, consider the following
// sequence of key-value pairs:
//
// simple_key = simple_value
// thing.key = value
// thing.point[0].x = 10
// thing.point[0].y = 20
// thing.point[1].x = 0
// thing.point[1].y = -10
//
// It's going to be converted to a following object:
//
// {
//   "simple_key": "simple_value",
//   "thing": {
//     "key": "value",
//     "point": [
//       { "x": "10", "y": "20" },
//       { "x": "0", "y": "-10" }
//     ]
//   }
// }
export function parseArgs(args: Args): ArgsTree|undefined {
  const result: ArgsTreeMap = {};
  for (const [key, value] of args) {
    if (typeof value === 'string') {
      fillObject(result, key.split('.'), value);
    }
  }
  return result;
}

function getOrCreateMap(
    object: ArgsTreeMap|ArgsTreeArray, key: string|number): ArgsTreeMap {
  let value: ArgsTree;
  if (isArgTreeMap(object) && typeof key === 'string') {
    value = object[key];
  } else if (isArgTreeArray(object) && typeof key === 'number') {
    value = object[key];
  } else {
    throw new Error('incompatible parameters to getOrCreateSubmap');
  }

  if (value !== undefined) {
    if (isArgTreeMap(value)) {
      return value;
    } else {
      // There is a value, but it's not a map - something wrong with the key set
      throw new Error('inconsistent keys');
    }
  }

  value = {};
  if (isArgTreeMap(object) && typeof key === 'string') {
    object[key] = value;
  } else if (isArgTreeArray(object) && typeof key === 'number') {
    object[key] = value;
  }

  return value;
}

function getOrCreateArray(object: ArgsTreeMap, key: string): ArgsTree[] {
  let value = object[key];
  if (value !== undefined) {
    if (isArgTreeArray(value)) {
      return value;
    } else {
      // There is a value, but it's not an array - something wrong with the key
      // set
      throw new Error('inconsistent keys');
    }
  }

  value = [];
  object[key] = value;
  return value;
}

function fillObject(object: ArgsTreeMap, path: string[], value: string) {
  let current = object;
  for (let i = 0; i < path.length - 1; i++) {
    const [part, index] = parsePathSegment(path[i]);
    if (index === undefined) {
      current = getOrCreateMap(current, part);
    } else {
      const array = getOrCreateArray(current, part);
      current = getOrCreateMap(array, index);
    }
  }

  const [part, index] = parsePathSegment(path[path.length - 1]);
  if (index === undefined) {
    current[part] = value;
  } else {
    const array = getOrCreateArray(current, part);
    array[index] = value;
  }
}

// Segment is either a simple key (e.g. "foo") or a key with an index (e.g.
// "bar[42]"). This function returns a pair of key and index (if present).
function parsePathSegment(segment: string): [string, number?] {
  if (!segment.endsWith(']')) {
    return [segment, undefined];
  }

  const indexStart = segment.indexOf('[');
  const indexString = segment.substring(indexStart + 1, segment.length - 1);
  return [segment.substring(0, indexStart), Math.floor(Number(indexString))];
}
