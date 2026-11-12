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

import type m from 'mithril';
import {showModal} from '../../widgets/modal';

export interface ModalPromptButton<T extends string> {
  // The button's label.
  readonly text: string;
  // What promptModal() resolves with if this button is pressed.
  readonly value: T;
  readonly primary?: boolean;
}

export interface ModalPromptArgs<T extends string> {
  readonly title: string;
  readonly content: m.Children | (() => m.Children);
  readonly buttons: ReadonlyArray<ModalPromptButton<T>>;
  // See ModalAttrs.key.
  readonly key?: string;
}

// Shows a modal dialog and waits for the user to respond to it.
//
// Resolves with the `value` of the button the user pressed, or undefined if
// they dismissed the dialog some other way (Escape, or its close button). This
// lets a flow be written top to bottom, rather than as callbacks on buttons.
//
// Note that this does not resolve at the moment of the click: showModal() only
// settles once the dialog's fade-out animation has finished, about a quarter
// of a second later. Code that follows the await is therefore not running
// inside the click handler. Opening a popup from there still works in Chrome,
// which allows it for a few seconds after a click, and callers rely on that.
export async function promptModal<T extends string>(
  args: ModalPromptArgs<T>,
): Promise<T | undefined> {
  let pressed: T | undefined;
  await showModal({
    key: args.key,
    title: args.title,
    content: args.content,
    buttons: args.buttons.map((button) => ({
      text: button.text,
      primary: button.primary,
      action: () => {
        pressed = button.value;
      },
    })),
  });
  return pressed;
}

// Shows a modal dialog with a message and a single "OK" button, and waits for
// the user to dismiss it.
export async function alertModal(args: {
  readonly title: string;
  readonly content: m.Children;
  readonly key?: string;
}): Promise<void> {
  await promptModal({
    ...args,
    buttons: [{text: 'OK', value: 'ok', primary: true}],
  });
}
