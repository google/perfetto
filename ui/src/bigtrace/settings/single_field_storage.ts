// Copyright (C) 2026 The Android Open Source Project
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

import {LocalStorage} from '../../core/local_storage';

// LocalStorage-backed holder for a single persisted field, shared by the
// trace-selection states; each supplies its key/field/parse/clear-default
// (traceColumnsState adds effective() in a subclass to reconcile against a live
// schema). save() JSON-serializes, so callers needn't copy arrays before set().
export class SingleFieldStorage<T> {
  private readonly storage: LocalStorage;

  constructor(
    key: string,
    private readonly field: string,
    private readonly parse: (raw: unknown) => T,
    private readonly clearValue: T,
  ) {
    this.storage = new LocalStorage(key);
  }

  get(): T {
    return this.parse(this.storage.load()[this.field]);
  }

  set(value: T): void {
    this.storage.save({[this.field]: value});
  }

  clear(): void {
    this.set(this.clearValue);
  }
}

// Shared parse for the nullable string[] column states: persisted list filtered
// to strings, or null for nothing-stored / malformed / empty (null = use
// defaults, since a zero-column grid is never what the user wants).
export function parseNullableStringArray(
  raw: unknown,
): readonly string[] | null {
  if (!Array.isArray(raw)) return null;
  const filtered = raw.filter((v): v is string => typeof v === 'string');
  return filtered.length === 0 ? null : filtered;
}
