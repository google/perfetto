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

import m from 'mithril';
import {Icons} from '../../../base/semantic_icons';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Form, FormLabel, FormSection} from '../../../widgets/form';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {Select} from '../../../widgets/select';
import {TextInput} from '../../../widgets/text_input';
import {renderDocSection, renderWidgetShowcase} from '../widgets_page_utils';
import {Checkbox} from '../../../widgets/checkbox';
import {Switch} from '../../../widgets/switch';

function renderFormContent(
  id: string,
  options: {
    submitButton?: boolean;
    cancelButton?: boolean;
    resetButton?: boolean;
  } = {},
): m.Children {
  const {submitButton, cancelButton, resetButton} = options;
  return m(
    Form,
    {
      submitLabel: submitButton ? 'Submit' : undefined,
      submitIcon: 'send',
      cancelLabel: cancelButton ? 'Cancel' : undefined,
      resetLabel: resetButton ? 'Reset' : undefined,
      onSubmit: () => window.alert('Form submitted!'),
    },
    m(FormLabel, {for: `${id}-text-input`}, 'Text Input'),
    m(TextInput, {
      id: `${id}-text-input`,
      placeholder: 'Enter some text...',
    }),
    m(FormLabel, {for: `${id}-select`}, 'Select'),
    m(Select, {id: `${id}-select`}, [
      m('option', {value: 'foo', label: 'Foo'}),
      m('option', {value: 'bar', label: 'Bar'}),
      m('option', {value: 'baz', label: 'Baz'}),
    ]),
    m(FormLabel, {for: `${id}-required-text`}, 'Required Text (*)'),
    m(TextInput, {
      id: `${id}-required-text`,
      required: true,
      placeholder: 'This field is required',
    }),
    m(
      FormSection,
      {label: 'Form Section'},
      m(FormLabel, {for: `${id}-email`}, 'Email (*)'),
      m(TextInput, {
        id: `${id}-email`,
        type: 'email',
        required: true,
        placeholder: 'Enter a valid email',
      }),
      m(
        FormSection,
        {label: 'Nested form section'},
        m(FormLabel, {for: `${id}-pattern`}, 'Pattern (5 digits)'),
        m(TextInput, {
          id: `${id}-pattern`,
          pattern: '[0-9]{5}',
          placeholder: 'Enter exactly 5 digits',
          title: 'Please enter exactly 5 digits',
        }),
      ),
      m(FormLabel, {for: `${id}-required-select`}, 'Required Select (*)'),
      m(Select, {id: `${id}-required-select`, required: true}, [
        m('option', {value: '', label: '-- Select an option --'}),
        m('option', {value: 'option1', label: 'Option 1'}),
        m('option', {value: 'option2', label: 'Option 2'}),
        m('option', {value: 'option3', label: 'Option 3'}),
      ]),
      m(Checkbox, {
        label: 'I agree to the terms and conditions',
        id: `${id}-checkbox`,
      }),
      m(Switch, {
        label: 'Enable notifications',
        id: `${id}-switch`,
      }),
    ),
  );
}

export function renderForm(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Form'),
      m(
        'p',
        'A form container component for organizing input fields with labels, validation, and layout utilities.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: ({submitButton, cancelButton, resetButton}) =>
        renderFormContent('inline-form', {
          submitButton,
          cancelButton,
          resetButton,
        }),
      initialOpts: {
        submitButton: true,
        cancelButton: true,
        resetButton: false,
      },
    }),

    renderDocSection('Form with a Popup', [
      m('p', [
        `A form placed inside a popup menu works just fine,
         and the cancel/submit buttons also dismiss the popup. A bit more
         margin is added around it too, which improves the look and feel.`,
      ]),
    ]),

    renderWidgetShowcase({
      renderWidget: () =>
        m(
          PopupMenu,
          {
            trigger: m(Button, {
              label: 'Click me',
              icon: Icons.ContextMenu,
              variant: ButtonVariant.Filled,
            }),
          },
          m(
            MenuItem,
            {
              label: 'Open form...',
            },
            renderFormContent('popup-form'),
          ),
        ),
    }),
  ];
}
