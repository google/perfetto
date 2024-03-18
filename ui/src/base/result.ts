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

export enum ResultStatus {
  PENDING = 'pending',
  SUCCESS = 'success',
  ERROR = 'error',
}

export interface PendingResult {
  status: ResultStatus.PENDING;
}

export interface ErrorResult {
  status: ResultStatus.ERROR;
  error: string;
}

export interface SuccessResult<T> {
  status: ResultStatus.SUCCESS;
  data: T;
}

export type Result<T> = PendingResult | ErrorResult | SuccessResult<T>;

export function isError<T>(result: Result<T>): result is ErrorResult {
  return result.status === ResultStatus.ERROR;
}

export function isPending<T>(result: Result<T>): result is PendingResult {
  return result.status === ResultStatus.PENDING;
}

export function isSuccess<T>(result: Result<T>): result is SuccessResult<T> {
  return result.status === ResultStatus.SUCCESS;
}

export function pending(): PendingResult {
  return {status: ResultStatus.PENDING};
}

export function error(message: string): ErrorResult {
  return {
    status: ResultStatus.ERROR,
    error: message,
  };
}

export function success<T>(data: T): SuccessResult<T> {
  return {
    status: ResultStatus.SUCCESS,
    data,
  };
}
