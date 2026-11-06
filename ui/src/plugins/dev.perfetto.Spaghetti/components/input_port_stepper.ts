// Copyright (C) 2026 The Android Open Source Project
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
import {Button, ButtonVariant} from '../../../widgets/button';

export interface InputPortStepperAttrs {
  readonly canRemove: boolean;
  readonly onAdd: () => void;
  readonly onRemove: () => void;
}

export class InputPortStepper implements m.ClassComponent<InputPortStepperAttrs> {
  view({attrs}: m.CVnode<InputPortStepperAttrs>): m.Children {
    const {canRemove, onAdd, onRemove} = attrs;
    return m('.pf-spag-port-stepper', [
      canRemove &&
        m(Button, {
          label: '− Input',
          variant: ButtonVariant.Filled,
          onclick: onRemove,
        }),
      m(Button, {
        label: '+ Input',
        variant: ButtonVariant.Filled,
        onclick: onAdd,
      }),
    ]);
  }
}
