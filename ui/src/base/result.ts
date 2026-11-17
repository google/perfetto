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

import {getErrorMessage} from './errors';

// Generic result type - similar to Rust's Result<T, E> or ABSL's StatusOr<T>.

export interface ErrorResult<E = string> {
  ok: false;
  error: E;
  value: undefined;
}

export interface OkResult<T> {
  ok: true;
  value: T;
}

export type Result<T = void, E = string> = ErrorResult<E> | OkResult<T>;

export function errResult<E = string>(error: E): ErrorResult<E> {
  return {ok: false, error, value: undefined};
}

export function okResult(): OkResult<void>;
export function okResult<T>(value: T): OkResult<T>;
export function okResult<T>(value?: T): OkResult<T | void> {
  return {ok: true, value};
}

// Returns the value of a successful result, or throws if it is an error.
//
// What is thrown depends on the error:
// - an Error is thrown as it is, keeping its stack and type;
// - a string becomes the message of a new Error;
// - anything else (e.g. a typed error object) is described in the message of
//   a new Error, and attached to it as its `cause` so nothing is lost.
export function unwrapResult<T, E = string>(result: Result<T, E>): T {
  if (result.ok) {
    return result.value;
  }
  const error: unknown = result.error;
  if (error instanceof Error) {
    throw error;
  }
  if (typeof error === 'string') {
    throw new Error(error);
  }
  throw new Error(getErrorMessage(error), {cause: error});
}
