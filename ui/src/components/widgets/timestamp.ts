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
import {assertExists} from '../../base/logging';
import {Icons} from '../../base/semantic_icons';
import {time, Time} from '../../base/time';
import {AppImpl} from '../../core/app_impl';
import {Anchor} from '../../widgets/anchor';
import {MenuDivider, MenuItem, PopupMenu} from '../../widgets/menu';
import {Trace} from '../../public/trace';
import {TimestampFormatMenuItem} from './timestamp_format_menu';
import {renderTimecode} from '../time_utils';
import {TimestampFormat} from '../../public/timeline';

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
      PopupMenu,
      {
        trigger: m(
          Anchor,
          {
            onmouseover: () => (timeline.hoverCursorTimestamp = ts),
            onmouseout: () => (timeline.hoverCursorTimestamp = undefined),
          },
          attrs.display ?? this.formatTimestamp(timeline.toDomainTime(ts)),
        ),
      },
      m(MenuItem, {
        icon: Icons.Copy,
        label: `Copy raw value`,
        onclick: () => {
          copyToClipboard(ts.toString());
        },
      }),
      m(TimestampFormatMenuItem, {trace: this.trace}),
      attrs.extraMenuItems ? [m(MenuDivider), attrs.extraMenuItems] : null,
    );
  }

  private formatTimestamp(time: time): m.Children {
    const fmt = this.trace.timeline.timestampFormat;
    switch (fmt) {
      case TimestampFormat.UTC:
      case TimestampFormat.TraceTz:
      case TimestampFormat.Timecode:
        return renderTimecode(time);
      case TimestampFormat.TraceNs:
        return time.toString();
      case TimestampFormat.TraceNsLocale:
        return time.toLocaleString();
      case TimestampFormat.Seconds:
        return Time.formatSeconds(time);
      case TimestampFormat.Milliseconds:
        return Time.formatMilliseconds(time);
      case TimestampFormat.Microseconds:
        return Time.formatMicroseconds(time);
      default:
        const x: never = fmt;
        throw new Error(`Invalid timestamp ${x}`);
    }
  }
}
