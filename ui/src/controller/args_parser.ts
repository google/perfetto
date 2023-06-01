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

export type Key = string|number;

export interface Argument<T> {
  key: Key;
  path?: string;
  value?: T;
  children?: Argument<T>[];
}

// Converts a flats sequence of key-value pairs into a JSON-like nested
// structure. Dots in keys are used to create a nested dictionary, indices in
// brackets used to create nested array.
export function convertArgsToTree<T>(input: Map<string, T>): Argument<T>[] {
  const result: Argument<T>[] = [];
  for (const [path, value] of input.entries()) {
    const nestedKey = getNestedKey(path);
    insert(result, nestedKey, path, value);
  }
  return result;
}

function getNestedKey(key: string): Key[] {
  const result: Key[] = [];
  let match;
  const re = /([^\.\[\]]+)|\[(\d+)\]/g;
  while ((match = re.exec(key)) !== null) {
    result.push(match[2] ? parseInt(match[2]) : match[1]);
  }
  return result;
}

function insert<T>(
    args: Argument<T>[], keys: Key[], path: string, value: T): void {
  const currentKey = keys.shift()!;
  let node = args.find((x) => x.key === currentKey);
  if (!node) {
    node = {key: currentKey};
    args.push(node);
  }
  if (keys.length > 0) {
    if (node.children === undefined) {
      node.children = [];
    }
    insert(node.children, keys, path, value);
  } else {
    node.path = path;
    node.value = value;
  }
}
