// Copyright (C) 2022 The Android Open Source Project
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

// Simple builder-style class to implement object equality more succinctly.
export class EqualsBuilder<T> {
  result = true;
  first: T;
  second: T;

  constructor(first: T, second: T) {
    this.first = first;
    this.second = second;
  }

  comparePrimitive(getter: (arg: T) => string | number): EqualsBuilder<T> {
    if (this.result) {
      this.result = getter(this.first) === getter(this.second);
    }
    return this;
  }

  compare<S>(
      comparator: (first: S, second: S) => boolean,
      getter: (arg: T) => S): EqualsBuilder<T> {
    if (this.result) {
      this.result = comparator(getter(this.first), getter(this.second));
    }
    return this;
  }

  equals(): boolean {
    return this.result;
  }
}
