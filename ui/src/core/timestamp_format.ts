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

import {isEnumValue} from '../base/object_utils';

export enum TimestampFormat {
  Timecode = 'timecode',
  TraceNs = 'traceNs',
  TraceNsLocale = 'traceNsLocale',
  Seconds = 'seconds',
  Milliseoncds = 'milliseconds',
  Microseconds = 'microseconds',
  UTC = 'utc',
  TraceTz = 'traceTz',
}

let timestampFormatCached: TimestampFormat | undefined;

const TIMESTAMP_FORMAT_KEY = 'timestampFormat';
const DEFAULT_TIMESTAMP_FORMAT = TimestampFormat.Timecode;

export function timestampFormat(): TimestampFormat {
  if (timestampFormatCached !== undefined) {
    return timestampFormatCached;
  } else {
    const storedFormat = localStorage.getItem(TIMESTAMP_FORMAT_KEY);
    if (storedFormat && isEnumValue(TimestampFormat, storedFormat)) {
      timestampFormatCached = storedFormat;
    } else {
      timestampFormatCached = DEFAULT_TIMESTAMP_FORMAT;
    }
    return timestampFormatCached;
  }
}

export function setTimestampFormat(format: TimestampFormat) {
  timestampFormatCached = format;
  localStorage.setItem(TIMESTAMP_FORMAT_KEY, format);
}

export enum DurationPrecision {
  Full = 'full',
  HumanReadable = 'human_readable',
}

let durationFormatCached: DurationPrecision | undefined;

const DURATION_FORMAT_KEY = 'durationFormat';
const DEFAULT_DURATION_FORMAT = DurationPrecision.Full;

export function durationPrecision(): DurationPrecision {
  if (durationFormatCached !== undefined) {
    return durationFormatCached;
  } else {
    const storedFormat = localStorage.getItem(DURATION_FORMAT_KEY);
    if (storedFormat && isEnumValue(DurationPrecision, storedFormat)) {
      durationFormatCached = storedFormat;
    } else {
      durationFormatCached = DEFAULT_DURATION_FORMAT;
    }
    return durationFormatCached;
  }
}

export function setDurationPrecision(format: DurationPrecision) {
  durationFormatCached = format;
  localStorage.setItem(DURATION_FORMAT_KEY, format);
}
