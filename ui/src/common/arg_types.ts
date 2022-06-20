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

export type Arg = string|
    {kind: 'SLICE', trackId: string, sliceId: number, description?: string};
export type Args = Map<string, Arg>;

export type ArgsTree = ArgsTreeMap|ArgsTreeArray|string;
export type ArgsTreeArray = ArgsTree[];
export interface ArgsTreeMap {
  [key: string]: ArgsTree;
}

export function isArgTreeArray(item: ArgsTree): item is ArgsTreeArray {
  return typeof item === 'object' && Array.isArray(item);
}

export function isArgTreeMap(item: ArgsTree): item is ArgsTreeMap {
  return typeof item === 'object' && !Array.isArray(item);
}
