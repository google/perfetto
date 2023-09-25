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
import {Actions} from '../../common/actions';
import {TimestampFormat, timestampFormat} from '../../common/timestamp_format';
import {Anchor} from '../../widgets/anchor';
import {MenuItem, PopupMenu2} from '../../widgets/menu';
import {globals} from '../globals';

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
  view({attrs}: m.Vnode<TimestampAttrs>) {
    const {ts} = attrs;
    return m(
        PopupMenu2,
        {
          trigger:
              m(Anchor,
                {
                  onmouseover: () => {
                    globals.dispatch(Actions.setHoverCursorTimestamp({ts}));
                  },
                  onmouseout: () => {
                    globals.dispatch(
                        Actions.setHoverCursorTimestamp({ts: Time.INVALID}));
                  },
                },
                attrs.display ?? renderTimestamp(ts)),
        },
        m(MenuItem, {
          icon: Icons.Copy,
          label: `Copy raw value`,
          onclick: () => {
            copyToClipboard(ts.toString());
          },
        }),
        ...(attrs.extraMenuItems ?? []),
    );
  }
}

function renderTimestamp(time: time): m.Children {
  const fmt = timestampFormat();
  const domainTime = globals.toDomainTime(time);
  switch (fmt) {
    case TimestampFormat.UTC:
    case TimestampFormat.Timecode:
      return renderTimecode(domainTime);
    case TimestampFormat.Raw:
      return domainTime.toString();
    case TimestampFormat.RawLocale:
      return domainTime.toLocaleString();
    case TimestampFormat.Seconds:
      return Time.formatSeconds(domainTime);
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
