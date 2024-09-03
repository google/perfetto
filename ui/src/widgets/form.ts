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
import {Button} from './button';
import {HTMLAttrs, HTMLLabelAttrs} from './common';
import {Popup} from './popup';
import {Intent} from '../widgets/common';

export interface FormAttrs extends HTMLAttrs {
  // Text to show on the "submit" button.
  // Defaults to "Submit".
  submitLabel?: string;

  // Icon to show on the "submit" button.
  submitIcon?: string;

  // Text to show on the "cancel" button.
  // No button is rendered if this value is omitted.
  cancelLabel?: string;

  // Text to show on the "reset" button.
  // No button is rendered if this value is omitted.
  resetLabel?: string;

  // Action to take when the form is submitted either by the enter key or
  // the submit button.
  onSubmit?: () => void;

  // Action to take when the form is cancelled.
  onCancel?: () => void;

  // Prevent default form action on submit. Defaults to true.
  preventDefault?: boolean;
}

// A simple wrapper around a <form> element providing some opinionated default
// buttons and form behavior. Designed to be used with FormLabel elements.
// Can be used in popups and popup menus and pressing either of the cancel or
// submit buttons dismisses the popup.
// See Widgets page for examples.
export class Form implements m.ClassComponent<FormAttrs> {
  view({attrs, children}: m.CVnode<FormAttrs>) {
    const {
      submitIcon = undefined,
      submitLabel = 'Submit',
      cancelLabel,
      resetLabel,
      onSubmit = () => {},
      preventDefault = true,
      ...htmlAttrs
    } = attrs;

    return m(
      'form.pf-form',
      htmlAttrs,
      children,
      m(
        '.pf-form-button-bar',
        m(Button, {
          type: 'submit',
          label: submitLabel,
          rightIcon: submitIcon,
          className: Popup.DISMISS_POPUP_GROUP_CLASS,
          intent: Intent.Primary,
          onclick: (e: Event) => {
            preventDefault && e.preventDefault();
            onSubmit();
          },
        }),
        // This cancel button just closes the popup if we are inside one.
        cancelLabel &&
          m(Button, {
            type: 'button',
            label: cancelLabel,
            className: Popup.DISMISS_POPUP_GROUP_CLASS,
          }),
        // This reset button just clears the form.
        resetLabel &&
          m(Button, {
            label: resetLabel,
            type: 'reset',
          }),
      ),
    );
  }
}

// A simple wrapper around a <label> element. Designed to be used within Form
// widgets in combination with input controls to provide consistent label
// styling.
//
// Like normal labels, FormLabels provide a name for an input while also
// improving their hit area which improves a11y.
//
// Labels are bound to inputs by placing the input inside the FormLabel widget,
// or by referencing the input's "id" tag with a "for" tag.
export class FormLabel implements m.ClassComponent<HTMLLabelAttrs> {
  view({attrs, children}: m.CVnode<HTMLLabelAttrs>) {
    return m('label.pf-form-label', attrs, children);
  }
}
