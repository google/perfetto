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
import {DurationPrecision, TimestampFormat} from '../../public/timeline';

interface DurationPrecisionMenuItemAttrs {
  trace: Trace;
}

export class DurationPrecisionMenuItem
  implements m.ClassComponent<DurationPrecisionMenuItemAttrs>
{
  view({attrs}: m.Vnode<DurationPrecisionMenuItemAttrs>) {
    function renderMenuItem(value: DurationPrecision, label: string) {
      return m(MenuItem, {
        label,
        active: value === attrs.trace.timeline.durationPrecision,
        onclick: () => {
          attrs.trace.timeline.durationPrecision = value;
        },
      });
    }

    function durationPrecisionHasEffect() {
      switch (attrs.trace.timeline.timestampFormat) {
        case TimestampFormat.Timecode:
        case TimestampFormat.UTC:
        case TimestampFormat.TraceTz:
          return true;
        default:
          return false;
      }
    }

    return m(
      MenuItem,
      {
        label: 'Duration precision',
        disabled: !durationPrecisionHasEffect(),
        title: 'Not configurable with current time format',
      },
      renderMenuItem(DurationPrecision.Full, 'Full'),
      renderMenuItem(DurationPrecision.HumanReadable, 'Human readable'),
    );
  }
}
