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

// Generic result type - similar to Rust's Result<T, E> or ABSL's StatusOr<T>.

export interface ErrorResult {
  ok: false;
  error: string;
  value: undefined;
}

export interface OkResult<T> {
  ok: true;
  value: T;
}

export type Result<T = void> = ErrorResult | OkResult<T>;

export function errResult(message: string): ErrorResult {
  return {ok: false, error: message, value: undefined};
}

export function okResult(): OkResult<void>;
export function okResult<T>(value: T): OkResult<T>;
export function okResult<T>(value?: T): OkResult<T | void> {
  return {ok: true, value};
}

export function unwrapResult<T>(result: Result<T>): T {
  if (!result.ok) {
    throw new Error(result.error);
  }
  return result.value;
}
