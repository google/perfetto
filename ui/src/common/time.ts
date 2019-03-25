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
  const units = ['s', 'ms', 'us', 'ns'];
  const sign = Math.sign(sec);
  let n = Math.abs(sec);
  let u = 0;
  while (n < 1 && n !== 0 && u < units.length - 1) {
    n *= 1000;
    u++;
  }
  return `${sign < 0 ? '-' : ''}${Math.round(n * 10) / 10} ${units[u]}`;
}

export function fromNs(ns: number) {
  return ns / 1e9;
}

export function timeToCode(sec: number) {
  let result = '';
  let ns = Math.round(sec * 1e9);
  if (ns < 1) return '0s ';
  const unitAndValue = [
    ['m', 60000000000],
    ['s', 1000000000],
    ['ms', 1000000],
    ['us', 1000],
    ['ns', 1]
  ];
  unitAndValue.forEach(pair => {
    const unit = pair[0] as string;
    const val = pair[1] as number;
    if (ns >= val) {
      const i = Math.floor(ns / val);
      ns -= i * val;
      result += i.toLocaleString() + unit + ' ';
    }
  });
  return result;
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

  isInBounds(sec: number) {
    return this.start <= sec && sec < this.end;
  }

  add(sec: number): TimeSpan {
    return new TimeSpan(this.start + sec, this.end + sec);
  }
}
