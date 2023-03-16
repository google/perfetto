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

import * as m from 'mithril';
import {classNames} from '../classnames';

export interface IconAttrs {
  // The material icon name.
  icon: string;
  // Whether to show the filled version of the icon.
  // Defaults to false.
  filled?: boolean;
  // List of space separated class names forwarded to the icon.
  className?: string;
}

export class Icon implements m.ClassComponent<IconAttrs> {
  view(vnode: m.Vnode<IconAttrs>): m.Child {
    const classes = classNames(vnode.attrs.className);
    return m(
        vnode.attrs.filled ? 'i.material-icons-filled' : 'i.material-icons',
        {class: classes},
        vnode.attrs.icon);
  }
}
