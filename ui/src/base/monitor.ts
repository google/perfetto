// Copyright (C) 2024 The Android Open Source Project
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

type Reducer = () => unknown;
type Callback = () => void;

/**
 * A little helper that monitors a list of immutable objects and calls a
 * callback only when at least one them changes.
 */
export class Monitor {
  private cached: unknown[];

  constructor(private reducers: Reducer[]) {
    this.cached = reducers.map(() => undefined);
  }

  ifStateChanged(callback: Callback): void {
    const state = this.reducers.map((f) => f());
    if (state.some((x, i) => x !== this.cached[i])) {
      callback();
    }
    this.cached = state;
  }
}
