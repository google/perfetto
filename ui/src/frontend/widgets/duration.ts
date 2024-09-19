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

import m from 'mithril';
import {copyToClipboard} from '../../base/clipboard';
import {Icons} from '../../base/semantic_icons';
import {Duration, duration} from '../../base/time';
import {
  DurationPrecision,
  durationPrecision,
  setDurationPrecision,
  TimestampFormat,
  timestampFormat,
} from '../../core/timestamp_format';
import {raf} from '../../core/raf_scheduler';
import {Anchor} from '../../widgets/anchor';
import {MenuDivider, MenuItem, PopupMenu2} from '../../widgets/menu';
import {menuItemForFormat} from './timestamp';

interface DurationWidgetAttrs {
  dur: duration;
  extraMenuItems?: m.Child[];
}

export class DurationWidget implements m.ClassComponent<DurationWidgetAttrs> {
  view({attrs}: m.Vnode<DurationWidgetAttrs>) {
    const {dur} = attrs;
    if (dur === -1n) {
      return '(Did not end)';
    }
    return m(
      PopupMenu2,
      {
        trigger: m(Anchor, renderDuration(dur)),
      },
      m(MenuItem, {
        icon: Icons.Copy,
        label: `Copy raw value`,
        onclick: () => {
          copyToClipboard(dur.toString());
        },
      }),
      m(
        MenuItem,
        {
          label: 'Set time format',
        },
        menuItemForFormat(TimestampFormat.Timecode, 'Timecode'),
        menuItemForFormat(TimestampFormat.UTC, 'Realtime (UTC)'),
        menuItemForFormat(TimestampFormat.TraceTz, 'Realtime (Trace TZ)'),
        menuItemForFormat(TimestampFormat.Seconds, 'Seconds'),
        menuItemForFormat(TimestampFormat.Milliseoncds, 'Milliseconds'),
        menuItemForFormat(TimestampFormat.Microseconds, 'Microseconds'),
        menuItemForFormat(TimestampFormat.TraceNs, 'Raw'),
        menuItemForFormat(
          TimestampFormat.TraceNsLocale,
          'Raw (with locale-specific formatting)',
        ),
      ),
      m(
        MenuItem,
        {
          label: 'Duration precision',
          disabled: !durationPrecisionHasEffect(),
          title: 'Not configurable with current time format',
        },
        menuItemForPrecision(DurationPrecision.Full, 'Full'),
        menuItemForPrecision(DurationPrecision.HumanReadable, 'Human readable'),
      ),
      attrs.extraMenuItems ? [m(MenuDivider), attrs.extraMenuItems] : null,
    );
  }
}

function menuItemForPrecision(
  value: DurationPrecision,
  label: string,
): m.Children {
  return m(MenuItem, {
    label,
    active: value === durationPrecision(),
    onclick: () => {
      setDurationPrecision(value);
      raf.scheduleFullRedraw();
    },
  });
}

function durationPrecisionHasEffect(): boolean {
  switch (timestampFormat()) {
    case TimestampFormat.Timecode:
    case TimestampFormat.UTC:
    case TimestampFormat.TraceTz:
      return true;
    default:
      return false;
  }
}

export function renderDuration(dur: duration): string {
  const fmt = timestampFormat();
  switch (fmt) {
    case TimestampFormat.UTC:
    case TimestampFormat.TraceTz:
    case TimestampFormat.Timecode:
      return renderFormattedDuration(dur);
    case TimestampFormat.TraceNs:
      return dur.toString();
    case TimestampFormat.TraceNsLocale:
      return dur.toLocaleString();
    case TimestampFormat.Seconds:
      return Duration.formatSeconds(dur);
    case TimestampFormat.Milliseoncds:
      return Duration.formatMilliseconds(dur);
    case TimestampFormat.Microseconds:
      return Duration.formatMicroseconds(dur);
    default:
      const x: never = fmt;
      throw new Error(`Invalid format ${x}`);
  }
}

function renderFormattedDuration(dur: duration): string {
  const fmt = durationPrecision();
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
