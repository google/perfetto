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
// eslint-disable-next-line @typescript-eslint/no-explicit-any
export function stringifyJsonWithBigints(object: any): string {
  return JSON.stringify(object, (_, value) =>
    typeof value === 'bigint' ? value.toString() : value,
  );
}
