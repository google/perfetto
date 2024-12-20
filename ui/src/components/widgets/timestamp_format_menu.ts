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

interface TimestampFormatMenuItemAttrs {
  trace: Trace;
}

export class TimestampFormatMenuItem
  implements m.ClassComponent<TimestampFormatMenuItemAttrs>
{
  view({attrs}: m.Vnode<TimestampFormatMenuItemAttrs>) {
    function renderMenuItem(value: TimestampFormat, label: string) {
      return m(MenuItem, {
        label,
        active: value === attrs.trace.timeline.timestampFormat,
        onclick: () => {
          attrs.trace.timeline.timestampFormat = value;
        },
      });
    }

    return m(
      MenuItem,
      {
        label: 'Time format',
      },
      renderMenuItem(TimestampFormat.Timecode, 'Timecode'),
      renderMenuItem(TimestampFormat.UTC, 'Realtime (UTC)'),
      renderMenuItem(TimestampFormat.TraceTz, 'Realtime (Trace TZ)'),
      renderMenuItem(TimestampFormat.Seconds, 'Seconds'),
      renderMenuItem(TimestampFormat.Milliseconds, 'Milliseconds'),
      renderMenuItem(TimestampFormat.Microseconds, 'Microseconds'),
      renderMenuItem(TimestampFormat.TraceNs, 'Raw'),
      renderMenuItem(
        TimestampFormat.TraceNsLocale,
        'Raw (with locale-specific formatting)',
      ),
    );
  }
}
