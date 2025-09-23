// Copyright (C) 2025 The Android Open Source Project
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

export type ParsedUrlParams = {
  [key: string]: string | boolean | (string | boolean)[];
};

/**
 * Parses URL search parameters with special handling for repeated arguments
 * and valueless arguments.
 * - Repeated search arguments are treated as an array of strings.
 * - Arguments without a value (e.g. "?foo") are treated as a boolean `true`.
 *
 * Example:
 * `?foo=bar&baz&baz=qux&a=1&a=2` will be parsed as:
 * `{foo: 'bar', baz: ['', 'qux'], a: ['1', '2']}`
 * A valueless parameter is only treated as true if it is not repeated.
 * e.g. `?foo` is parsed as `{foo: true}`.
 */
export function parseUrlSearchParams(params: URLSearchParams): ParsedUrlParams {
  const result: ParsedUrlParams = {};
  const keys = new Set<string>();
  for (const key of params.keys()) {
    keys.add(key);
  }

  const convert = (s: string): string | boolean => {
    if (s === '') return true;
    if (s === 'true') return true;
    if (s === 'false') return false;
    return s;
  };

  for (const key of keys) {
    const values = params.getAll(key);
    if (values.length > 1) {
      result[key] = values.map(convert);
    } else {
      result[key] = convert(values[0]);
    }
  }
  return result;
}

export function buildUrlSearchParams(params: ParsedUrlParams): string {
  const parts: string[] = [];
  for (const key of Object.keys(params).sort()) {
    const value = params[key];
    if (value === true) {
      parts.push(encodeURIComponent(key));
    } else if (value === false) {
      parts.push(`${encodeURIComponent(key)}=false`);
    } else if (typeof value === 'string') {
      parts.push(`${encodeURIComponent(key)}=${encodeURIComponent(value)}`);
    } else if (Array.isArray(value)) {
      for (const item of value) {
        const encodedKey = encodeURIComponent(key);
        if (item === true || item === '') {
          parts.push(encodedKey);
        } else if (item === false) {
          parts.push(`${encodedKey}=false`);
        } else {
          parts.push(`${encodedKey}=${encodeURIComponent(item as string)}`);
        }
      }
    }
  }
  return parts.join('&');
}
