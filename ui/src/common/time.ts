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

import {assertTrue} from '../base/logging';

export function timeToString(sec: number) {
  const units = ['ms', 'us', 'ns'];
  const sign = Math.sign(sec);
  let n = Math.abs(sec) * 1000;
  let u = 0;
  while (n < 1 && u !== 0 && u < units.length - 1) {
    n *= 1000;
    u++;
  }
  return `${sign < 0 ? '-' : ''}${Math.round(n * 1000) / 1000} ${units[u]}`;
}

export function fromNs(ns: number) {
  return ns / 1e9;
}

export class TimeSpan {
  readonly start: number;
  readonly end: number;

  constructor(start: number, end: number) {
    assertTrue(start <= end);
    this.start = start;
    this.end = end;
  }

  get duration() {
    return this.end - this.start;
  }

  add(sec: number): TimeSpan {
    return new TimeSpan(this.start + sec, this.end + sec);
  }
}
