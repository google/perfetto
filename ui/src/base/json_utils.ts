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

// Similar to JSON.stringify() but supports bigints.
// Bigints are simply serialized to a string, so the original object cannot be
// recovered with JSON.parse(), as bigints will turn into strings.
// Useful for e.g. tracing, where string arg values are required.
export function stringifyJsonWithBigints(
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  object: any,
  space?: string | number,
): string {
  return JSON.stringify(
    object,
    (_, value) => (typeof value === 'bigint' ? value.toString() : value),
    space,
  );
}

// Typescript bindings do not pass `context` to the reviver, so this helper works around that.
function parseJson(
  text: string,
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  reviver?: (key: string, value: any, context: {source: string}) => any,
) {
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  return JSON.parse(text, reviver as (key: string, value: any) => any);
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
export function parseJsonWithBigints(text: string): any {
  return parseJson(text, (_, value, context) => {
    if (typeof value === 'number') {
      // Even an integer value can be spelled as '1.0', which can't be converted to BigInt,
      // so we try converting sources for all values to BigInt and fallback to the original value if it fails.
      try {
        return BigInt(context.source);
      } catch (e) {
        return value;
      }
    }
    return value;
  });
}
