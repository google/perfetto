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

import {v4} from 'uuid';

export const uuidv4 = v4;

/**
 * Get a SQL friendly UUID, or convert a pre-existing one.
 * @param uuid Optional: Pre-existing uuid to format.
 * @returns string The resulting uuid.
 */
export function uuidv4Sql(uuid?: string): string {
  const str = uuid ?? uuidv4();
  return str.replace(/[^a-zA-Z0-9_]+/g, '_');
}
