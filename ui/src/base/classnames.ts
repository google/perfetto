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

// It's common to want to have a class depending on a boolean flag, in which
// case we use `flag && className` which evaluates to either false or a string,
// which is why false is included in definition of ArgType.
type ArgType = string | false | undefined | null;

// Join class names together into valid HTML class attributes
// Falsy elements are ignored
// If all elements are falsy, returns undefined
export function classNames(...args: ArgType[]) {
  // NOTE: This function is optimized for efficiency. Be very careful when
  // making changes & make sure to benchmark.
  let result = '';
  for (let i = 0; i < args.length; ++i) {
    // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
    if (args[i]) {
      result += result ? ' ' + args[i] : args[i];
    }
  }
  return result || undefined;
}
