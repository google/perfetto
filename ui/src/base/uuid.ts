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
import {sqlNameSafe} from './string_utils';

export const uuidv4 = v4;

/**
 * Get a SQL friendly UUID, or convert a pre-existing one.
 * @param uuid Optional: Pre-existing uuid to format.
 * @returns string The resulting uuid.
 */
export function uuidv4Sql(uuid?: string): string {
  const str = uuid ?? uuidv4();
  return sqlNameSafe(str);
}

// URL and SQL safe alphabet: A-Z, a-z, 0-9, _ (63 characters)
const ALPHABET =
  'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_';

/**
 * Generate a short unique ID suitable for column identifiers.
 * Prefixed with 'c' to ensure it's always a valid SQL identifier.
 * Uses 8 characters from a 63-char alphabet (~2^47.6 unique values).
 * @returns string A 9-character string starting with 'c'.
 */
export function shortUuid(): string {
  let result = 'c';
  for (let i = 0; i < 8; i++) {
    result += ALPHABET[Math.floor(Math.random() * ALPHABET.length)];
  }
  return result;
}
