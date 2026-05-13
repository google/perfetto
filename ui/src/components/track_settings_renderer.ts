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
import {TrackSettingDescriptor} from '../public/track';

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
  return descriptor.render(setter, values);
}
