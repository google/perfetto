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
import {Duration, duration, time, Time} from '../base/time';
import {Trace} from '../public/trace';
import {DurationPrecision, TimestampFormat} from '../public/timeline';

export function renderTimecode(time: time) {
  const {dhhmmss, millis, micros, nanos} = Time.toTimecode(time);
  return m(
    'span.pf-timecode',
    m('span.pf-timecode-hms', dhhmmss),
    '.',
    m('span.pf-timecode-millis', millis),
    m('span.pf-timecode-micros', micros),
    m('span.pf-timecode-nanos', nanos),
  );
}

export function formatDuration(trace: Trace, dur: duration): string {
  const fmt = trace.timeline.timestampFormat;
  switch (fmt) {
    case TimestampFormat.UTC:
    case TimestampFormat.TraceTz:
    case TimestampFormat.Timecode:
    case TimestampFormat.CustomTimezone:
      return renderFormattedDuration(trace, dur);
    case TimestampFormat.TraceNs:
      return dur.toString();
    case TimestampFormat.TraceNsLocale:
      return dur.toLocaleString();
    case TimestampFormat.Seconds:
      return Duration.formatSeconds(dur);
    case TimestampFormat.Milliseconds:
      return Duration.formatMilliseconds(dur);
    case TimestampFormat.Microseconds:
      return Duration.formatMicroseconds(dur);
    default:
      const x: never = fmt;
      throw new Error(`Invalid format ${x}`);
  }
}

function renderFormattedDuration(trace: Trace, dur: duration): string {
  const fmt = trace.timeline.durationPrecision;
  switch (fmt) {
    case DurationPrecision.HumanReadable:
      return Duration.humanise(dur);
    case DurationPrecision.Full:
      return Duration.format(dur);
    default:
      const x: never = fmt;
      throw new Error(`Invalid format ${x}`);
  }
}
