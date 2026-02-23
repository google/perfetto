// Copyright (C) 2025 The Android Open Source Project
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

// Given a track settings descriptor, renders a menu allowing the user to modify
// it. If a render option is already provided, it is used instead of the default
// rendering logic.

import m from 'mithril';
import {valueIfAllEqual} from '../base/array_utils';
import {getZodSchemaInfo} from '../base/zod_utils';
import {TrackSettingDescriptor} from '../public/track';
import {MenuItem} from '../widgets/menu';

/**
 * Infers the type of the setting from its descriptor and renders a suitable
 * menu item control for it.
 *
 * @param descriptor The track setting descriptor to generate a control for.
 * @param setter A function which can be called to set the value of the setting.
 * @param values A list of current setting values for the various different
 * tracks being edited. This will e multiple entries when bulk editing, and
 * only one for single track editing.
 */
export function renderTrackSettingMenu<T>(
  descriptor: TrackSettingDescriptor<T>,
  setter: (v: T) => void,
  values: ReadonlyArray<T>,
): m.Children {
  // If a renderer is provided, use it!
  if (descriptor.render) {
    return descriptor.render(setter, values);
  }

  const schemaInfo = getZodSchemaInfo(descriptor.schema);

  switch (schemaInfo.kind) {
    case 'boolean': {
      const value = valueIfAllEqual(values);
      const icon = (function () {
        switch (value) {
          case true:
            return 'check_box';
          case false:
            return 'check_box_outline_blank';
          default:
            return 'indeterminate_check_box'; // Mixed values
        }
      })();
      return m(MenuItem, {
        icon,
        label: descriptor.name,
        onclick: () => {
          switch (value) {
            case true:
              setter(false as T);
              break;
            case false:
            default:
              setter(true as T);
              break;
          }
        },
      });
    }

    case 'enum': {
      const value = valueIfAllEqual(values);
      return m(
        MenuItem,
        {
          label: `${descriptor.name} (currently: ${String(value)})`,
        },
        schemaInfo.options.map((option) => {
          return m(MenuItem, {
            label: option,
            active: value === option,
            onclick: () => setter(option as T),
          });
        }),
      );
    }

    case 'unknown':
    default:
      return m(MenuItem, {
        icon: 'error_outline',
        label: descriptor.name + ' - Cannot edit this setting directly',
      });
  }
}
