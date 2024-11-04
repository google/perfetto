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
import {time, Time} from '../../base/time';
import {
  setTimestampFormat,
  TimestampFormat,
  timestampFormat,
} from '../../core/timestamp_format';
import {raf} from '../../core/raf_scheduler';
import {Anchor} from '../../widgets/anchor';
import {MenuDivider, MenuItem, PopupMenu2} from '../../widgets/menu';
import {Trace} from '../../public/trace';
import {AppImpl} from '../../core/app_impl';
import {assertExists} from '../../base/logging';

// import {MenuItem, PopupMenu2} from './menu';

interface TimestampAttrs {
  // The timestamp to print, this should be the absolute, raw timestamp as
  // found in trace processor.
  ts: time;
  // Custom text value to show instead of the default HH:MM:SS.mmm uuu nnn
  // formatting.
  display?: m.Children;
  extraMenuItems?: m.Child[];
}

export class Timestamp implements m.ClassComponent<TimestampAttrs> {
  private readonly trace: Trace;

  constructor() {
    // TODO(primiano): the Trace object should be injected into the attrs, but
    // there are too many users of this class and doing so requires a larger
    // refactoring CL. Either that or we should find a different way to plumb
    // the hoverCursorTimestamp.
    this.trace = assertExists(AppImpl.instance.trace);
  }

  view({attrs}: m.Vnode<TimestampAttrs>) {
    const {ts} = attrs;
    const timeline = this.trace.timeline;
    return m(
      PopupMenu2,
      {
        trigger: m(
          Anchor,
          {
            onmouseover: () => (timeline.hoverCursorTimestamp = ts),
            onmouseout: () => (timeline.hoverCursorTimestamp = undefined),
          },
          attrs.display ?? renderTimestamp(timeline.toDomainTime(ts)),
        ),
      },
      m(MenuItem, {
        icon: Icons.Copy,
        label: `Copy raw value`,
        onclick: () => {
          copyToClipboard(ts.toString());
        },
      }),
      m(
        MenuItem,
        {
          label: 'Time format',
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
      attrs.extraMenuItems ? [m(MenuDivider), attrs.extraMenuItems] : null,
    );
  }
}

export function menuItemForFormat(
  value: TimestampFormat,
  label: string,
): m.Children {
  return m(MenuItem, {
    label,
    active: value === timestampFormat(),
    onclick: () => {
      setTimestampFormat(value);
      raf.scheduleFullRedraw();
    },
  });
}

function renderTimestamp(domainTime: time): m.Children {
  const fmt = timestampFormat();
  switch (fmt) {
    case TimestampFormat.UTC:
    case TimestampFormat.TraceTz:
    case TimestampFormat.Timecode:
      return renderTimecode(domainTime);
    case TimestampFormat.TraceNs:
      return domainTime.toString();
    case TimestampFormat.TraceNsLocale:
      return domainTime.toLocaleString();
    case TimestampFormat.Seconds:
      return Time.formatSeconds(domainTime);
    case TimestampFormat.Milliseoncds:
      return Time.formatMilliseconds(domainTime);
    case TimestampFormat.Microseconds:
      return Time.formatMicroseconds(domainTime);
    default:
      const x: never = fmt;
      throw new Error(`Invalid timestamp ${x}`);
  }
}

export function renderTimecode(time: time): m.Children {
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
