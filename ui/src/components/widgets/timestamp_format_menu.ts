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
import {MenuItem} from '../../widgets/menu';
import {Trace} from '../../public/trace';
import {TimestampFormat} from '../../public/timeline';
import {formatTimezone, timezoneOffsetMap} from '../../base/time';

interface TimestampFormatMenuItemAttrs {
  trace: Trace;
}

export class TimestampFormatMenuItem
  implements m.ClassComponent<TimestampFormatMenuItemAttrs>
{
  view({attrs}: m.Vnode<TimestampFormatMenuItemAttrs>) {
    const timeline = attrs.trace.timeline;
    function renderMenuItem(value: TimestampFormat, label: string) {
      return m(MenuItem, {
        label,
        active: value === timeline.timestampFormat,
        onclick: () => {
          timeline.timestampFormat = value;
        },
      });
    }

    const timeZone = formatTimezone(attrs.trace.traceInfo.tzOffMin);
    const TF = TimestampFormat;

    return m(
      MenuItem,
      {
        label: 'Time format',
      },
      renderMenuItem(TF.Timecode, 'Timecode'),
      renderMenuItem(TF.UTC, 'Realtime (UTC)'),
      renderMenuItem(TF.TraceTz, `Realtime (Trace TZ - ${timeZone})`),
      renderMenuItem(TF.Seconds, 'Seconds'),
      renderMenuItem(TF.Milliseconds, 'Milliseconds'),
      renderMenuItem(TF.Microseconds, 'Microseconds'),
      renderMenuItem(TF.TraceNs, 'Raw'),
      renderMenuItem(TF.TraceNsLocale, 'Raw (with locale-specific formatting)'),
      m(
        MenuItem,
        {
          label: 'Custom',
          active: TF.CustomTimezone === timeline.timestampFormat,
        },
        Object.keys(timezoneOffsetMap).map((tz) => {
          const customTz = timeline.timezoneOverride;
          return m(MenuItem, {
            label: tz,
            active: tz === customTz.get(),
            onclick: () => {
              timeline.timestampFormat = TF.CustomTimezone;
              customTz.set(tz);
            },
          });
        }),
      ),
    );
  }
}
