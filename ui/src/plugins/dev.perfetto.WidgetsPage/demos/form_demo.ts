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
import {Form, FormLabel} from '../../../widgets/form';
import {TextInput} from '../../../widgets/text_input';
import {Select} from '../../../widgets/select';
import {Button} from '../../../widgets/button';
import {PopupMenu} from '../../../widgets/menu';
import {MenuItem} from '../../../widgets/menu';
import {renderDocSection, renderWidgetShowcase} from '../widgets_page_utils';

function renderFormContent(id: string) {
  return m(
    Form,
    {
      submitLabel: 'Submit',
      submitIcon: 'send',
      cancelLabel: 'Cancel',
      resetLabel: 'Reset',
      onSubmit: () => window.alert('Form submitted!'),
    },
    m(FormLabel, {for: `${id}-foo`}, 'Foo'),
    m(TextInput, {id: `${id}-foo`}),
    m(FormLabel, {for: `${id}-bar`}, 'Bar'),
    m(Select, {id: `${id}-bar`}, [
      m('option', {value: 'foo', label: 'Foo'}),
      m('option', {value: 'bar', label: 'Bar'}),
      m('option', {value: 'baz', label: 'Baz'}),
    ]),
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
      renderWidget: () => renderFormContent('form'),
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
            trigger: m(Button, {label: 'Popup!'}),
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
