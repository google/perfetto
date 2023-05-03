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
import {classNames} from '../classnames';

export interface FormAttrs {
  // List of space separated class names forwarded to the icon.
  className?: string;
  // Remaining attributes forwarded to the underlying HTML <button>.
  [htmlAttrs: string]: any;
}

export class Form implements m.ClassComponent<FormAttrs> {
  view({attrs, children}: m.CVnode<FormAttrs>) {
    const {className, ...htmlAttrs} = attrs;

    const classes = classNames(
        'pf-form',
        className,
    );

    return m(
        'form.pf-form',
        {
          class: classes,
          ...htmlAttrs,
        },
        children,
    );
  }
}

export class FormButtonBar implements m.ClassComponent<{}> {
  view({children}: m.CVnode<{}>) {
    return m('.pf-form-button-bar', children);
  }
}

interface FormLabelAttrs {
  for: string;
  // Remaining attributes forwarded to the underlying HTML <button>.
  [htmlAttrs: string]: any;
}

export class FormLabel implements m.ClassComponent<FormLabelAttrs> {
  view({attrs, children}: m.CVnode<FormLabelAttrs>) {
    return m('label.pf-form-label', attrs, children);
  }
}
