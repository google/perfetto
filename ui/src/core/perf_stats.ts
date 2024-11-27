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

import m from 'mithril';

// The interface that every container (e.g. Track Panels) that exposes granular
// per-container masurements implements to be perf-stats-aware.
export interface PerfStatsContainer {
  setPerfStatsEnabled(enable: boolean): void;
  renderPerfStats(): m.Children;
}

// Stores statistics about samples, and keeps a fixed size buffer of most recent
// samples.
export class PerfStats {
  private _count = 0;
  private _mean = 0;
  private _lastValue = 0;
  private _ptr = 0;

  private buffer: number[] = [];

  constructor(private _maxBufferSize = 10) {}

  addValue(value: number) {
    this._lastValue = value;
    if (this.buffer.length >= this._maxBufferSize) {
      this.buffer[this._ptr++] = value;
      if (this._ptr >= this.buffer.length) {
        this._ptr -= this.buffer.length;
      }
    } else {
      this.buffer.push(value);
    }

    this._mean = (this._mean * this._count + value) / (this._count + 1);
    this._count++;
  }

  get mean() {
    return this._mean;
  }
  get count() {
    return this._count;
  }
  get bufferMean() {
    return this.buffer.reduce((sum, v) => sum + v, 0) / this.buffer.length;
  }
  get bufferSize() {
    return this.buffer.length;
  }
  get maxBufferSize() {
    return this._maxBufferSize;
  }
  get last() {
    return this._lastValue;
  }
}

// Returns a summary string representation of a RunningStatistics object.
export function runningStatStr(stat: PerfStats) {
  return (
    `Last: ${stat.last.toFixed(2)}ms | ` +
    `Avg: ${stat.mean.toFixed(2)}ms | ` +
    `Avg${stat.maxBufferSize}: ${stat.bufferMean.toFixed(2)}ms`
  );
}
