// Copyright (C) 2018 The Android Open Source Project
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

/**
 * A holder for a global variable.
 * This achieves similar goals to a singleton (the whole codebase can
 * access the same instance of a class) without some of the features/downsides
 * of a singleton, namely:
 * - The functionality isn't built into the class.
 * - Since initalization isn't lazy it must be done explicity which is easier
 *   to reason about.
 */
export class Global<T> {
  private value: T|null;

  constructor() {
    this.value = null;
  }

  get(): T {
    if (this.value === null) {
      throw new Error('Global not set');
    }
    return this.value;
  }

  set(value: T): void {
    this.value = value;
  }

  resetForTesting() {
    this.value = null;
  }
}
