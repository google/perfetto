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

export enum TimestampFormat {
  Timecode = 'timecode',
  Raw = 'raw',
  RawLocale = 'rawLocale',
  Seconds = 'seconds',
  UTC = 'utc',
}

let timestampFormatCached: TimestampFormat|undefined;

const TIMESTAMP_FORMAT_KEY = 'timestampFormat';
const DEFAULT_TIMESTAMP_FORMAT = TimestampFormat.Timecode;

function isTimestampFormat(value: unknown): value is TimestampFormat {
  return Object.values(TimestampFormat).includes(value as TimestampFormat);
}

export function timestampFormat(): TimestampFormat {
  const storedFormat = localStorage.getItem(TIMESTAMP_FORMAT_KEY);
  if (storedFormat && isTimestampFormat(storedFormat)) {
    timestampFormatCached = storedFormat;
  } else {
    timestampFormatCached = DEFAULT_TIMESTAMP_FORMAT;
  }
  return timestampFormatCached;
}

export function setTimestampFormat(format: TimestampFormat) {
  timestampFormatCached = format;
  localStorage.setItem(TIMESTAMP_FORMAT_KEY, format);
}
