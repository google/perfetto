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

import {DisposableStack} from '../base/disposable_stack';
import {OmniboxManager, PromptChoices} from '../public/omnibox';
import {raf} from './raf_scheduler';

export enum OmniboxMode {
  Search,
  Query,
  Command,
  Prompt,
}

interface Prompt {
  text: string;
  options?: ReadonlyArray<{key: string; displayName: string}>;
  resolve(result: unknown): void;
}

const defaultMode = OmniboxMode.Search;

export class OmniboxManagerImpl implements OmniboxManager {
  private _mode = defaultMode;
  private _focusOmniboxNextRender = false;
  private _pendingCursorPlacement?: number;
  private _pendingPrompt?: Prompt;
  private _omniboxSelectionIndex = 0;
  private _forceShortTextSearch = false;
  private _text = '';
  private _statusMessageContainer: {msg?: string} = {};
  private _promptsDisabled = false;

  get mode(): OmniboxMode {
    return this._mode;
  }

  get pendingPrompt(): Prompt | undefined {
    return this._pendingPrompt;
  }

  get text(): string {
    return this._text;
  }

  get selectionIndex(): number {
    return this._omniboxSelectionIndex;
  }

  get focusOmniboxNextRender(): boolean {
    return this._focusOmniboxNextRender;
  }

  get pendingCursorPlacement(): number | undefined {
    return this._pendingCursorPlacement;
  }

  get forceShortTextSearch() {
    return this._forceShortTextSearch;
  }

  setText(value: string): void {
    this._text = value;
  }

  setSelectionIndex(index: number): void {
    this._omniboxSelectionIndex = index;
  }

  focus(cursorPlacement?: number): void {
    this._focusOmniboxNextRender = true;
    this._pendingCursorPlacement = cursorPlacement;
  }

  clearFocusFlag(): void {
    this._focusOmniboxNextRender = false;
    this._pendingCursorPlacement = undefined;
  }

  setMode(mode: OmniboxMode, focus = true): void {
    this._mode = mode;
    this._focusOmniboxNextRender = focus;
    this._omniboxSelectionIndex = 0;
    this._text = '';
    this.rejectPendingPrompt();
  }

  showStatusMessage(msg: string, durationMs = 2000) {
    const statusMessageContainer: {msg?: string} = {msg};
    if (durationMs > 0) {
      setTimeout(() => {
        statusMessageContainer.msg = undefined;
        raf.scheduleFullRedraw();
      }, durationMs);
    }
    this._statusMessageContainer = statusMessageContainer;
  }

  get statusMessage(): string | undefined {
    return this._statusMessageContainer.msg;
  }

  // Start a prompt. If options are supplied, the user must pick one from the
  // list, otherwise the input is free-form text.
  prompt(text: string): Promise<string | undefined>;
  prompt(text: string, defaultValue: string): Promise<string | undefined>;
  prompt(
    text: string,
    choices: ReadonlyArray<string>,
  ): Promise<string | undefined>;
  prompt<T>(text: string, choices: PromptChoices<T>): Promise<T | undefined>;
  prompt<T>(
    text: string,
    choicesOrDefaultValue?: ReadonlyArray<string> | PromptChoices<T> | string,
  ): Promise<string | T | undefined> {
    if (this._promptsDisabled) {
      return Promise.resolve(undefined);
    }

    this._mode = OmniboxMode.Prompt;
    this._omniboxSelectionIndex = 0;
    this.rejectPendingPrompt();
    this._focusOmniboxNextRender = true;

    // Handle PromptChoices<T> case
    if (
      choicesOrDefaultValue !== undefined &&
      typeof choicesOrDefaultValue === 'object' &&
      'getName' in choicesOrDefaultValue
    ) {
      const choices = choicesOrDefaultValue as PromptChoices<T>;
      return new Promise<T | undefined>((resolve) => {
        const choiceMap = new Map(
          choices.values.map((choice) => [choices.getName(choice), choice]),
        );
        this._pendingPrompt = {
          text,
          options: Array.from(choiceMap.keys()).map((key) => ({
            key,
            displayName: key,
          })),
          resolve: (key: string) => resolve(choiceMap.get(key)),
        };
      });
    }

    // Handle ReadonlyArray<string> choices case
    if (
      choicesOrDefaultValue !== undefined &&
      Array.isArray(choicesOrDefaultValue)
    ) {
      const choices = choicesOrDefaultValue as ReadonlyArray<string>;
      return new Promise<string | undefined>((resolve) => {
        this._pendingPrompt = {
          text,
          options: choices.map((value) => ({key: value, displayName: value})),
          resolve,
        };
      });
    }

    // Handle free-form input (with or without default value)
    const defaultValue =
      typeof choicesOrDefaultValue === 'string'
        ? choicesOrDefaultValue
        : undefined;
    return new Promise<string | undefined>((resolve) => {
      if (defaultValue !== undefined) {
        this.setText(defaultValue);
      }
      this._pendingPrompt = {
        text,
        resolve,
      };
    });
  }

  // Resolve the pending prompt with a value to return to the prompter.
  resolvePrompt(value: string): void {
    if (this._pendingPrompt) {
      this._pendingPrompt.resolve(value);
      this._pendingPrompt = undefined;
    }
    this.setMode(OmniboxMode.Search);
  }

  // Reject the prompt outright. Doing this will force the owner of the prompt
  // promise to catch, so only do this when things go seriously wrong.
  // Use |resolvePrompt(null)| to indicate cancellation.
  rejectPrompt(): void {
    this.rejectPendingPrompt();
    this.setMode(OmniboxMode.Search);
  }

  reset(focus = true): void {
    this.setMode(defaultMode, focus);
    this._omniboxSelectionIndex = 0;
    this._statusMessageContainer = {};
  }

  disablePrompts(): Disposable {
    const trash = new DisposableStack();
    if (this._promptsDisabled) {
      return trash;
    }
    trash.defer(() => (this._promptsDisabled = false));
    this._promptsDisabled = true;
    this.rejectPendingPrompt();
    return trash;
  }

  private rejectPendingPrompt() {
    if (this._pendingPrompt) {
      this._pendingPrompt.resolve(undefined);
      this._pendingPrompt = undefined;
    }
  }
}
