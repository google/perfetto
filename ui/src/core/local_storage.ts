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

import {Storage} from './storage';

export class LocalStorage implements Storage {
  constructor(private readonly key: string) {}

  load(): Record<string, unknown> {
    const s = localStorage.getItem(this.key);
    let parsed: Record<string, unknown>;
    try {
      parsed = JSON.parse(s ?? '{}');
    } catch (e) {
      return {};
    }
    if (typeof parsed !== 'object' || parsed === null) {
      return {};
    }
    return parsed;
  }

  save(o: Record<string, unknown>): void {
    const s = JSON.stringify(o);
    localStorage.setItem(this.key, s);
  }
}
