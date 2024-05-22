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

import {raf} from '../core/raf_scheduler';

export enum OmniboxMode {
  Search,
  Query,
  Command,
  Prompt,
}

export interface PromptOption {
  key: string;
  displayName: string;
}

interface Prompt {
  text: string;
  options?: PromptOption[];
  resolve(result: string): void;
  reject(): void;
}

const defaultMode = OmniboxMode.Search;

export class OmniboxManager {
  private _omniboxMode = defaultMode;
  private _focusOmniboxNextRender = false;
  private _pendingCursorPlacement?: number;
  private _pendingPrompt?: Prompt;
  private _text = '';
  private _omniboxSelectionIndex = 0;

  get omniboxMode(): OmniboxMode {
    return this._omniboxMode;
  }

  get pendingPrompt(): Prompt | undefined {
    return this._pendingPrompt;
  }

  get text(): string {
    return this._text;
  }

  get omniboxSelectionIndex(): number {
    return this._omniboxSelectionIndex;
  }

  get focusOmniboxNextRender(): boolean {
    return this._focusOmniboxNextRender;
  }

  get pendingCursorPlacement(): number | undefined {
    return this._pendingCursorPlacement;
  }

  setText(value: string): void {
    this._text = value;
  }

  setOmniboxSelectionIndex(index: number): void {
    this._omniboxSelectionIndex = index;
  }

  focusOmnibox(cursorPlacement?: number): void {
    this._focusOmniboxNextRender = true;
    this._pendingCursorPlacement = cursorPlacement;
    raf.scheduleFullRedraw();
  }

  clearOmniboxFocusFlag(): void {
    this._focusOmniboxNextRender = false;
    this._pendingCursorPlacement = undefined;
  }

  setMode(mode: OmniboxMode): void {
    this._omniboxMode = mode;
    this._focusOmniboxNextRender = true;
    this.resetOmniboxText();
    this.rejectPendingPrompt();
    raf.scheduleFullRedraw();
  }

  // Start a prompt. If options are supplied, the user must pick one from the
  // list, otherwise the input is free-form text.
  prompt(text: string, options?: PromptOption[]): Promise<string> {
    this._omniboxMode = OmniboxMode.Prompt;
    this.resetOmniboxText();
    this.rejectPendingPrompt();

    const promise = new Promise<string>((resolve, reject) => {
      this._pendingPrompt = {
        text,
        options,
        resolve,
        reject,
      };
    });

    this._focusOmniboxNextRender = true;
    raf.scheduleFullRedraw();

    return promise;
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
    if (this._pendingPrompt) {
      this._pendingPrompt.reject();
      this._pendingPrompt = undefined;
    }
    this.setMode(OmniboxMode.Search);
  }

  reset(): void {
    this.setMode(defaultMode);
    this.resetOmniboxText();
    raf.scheduleFullRedraw();
  }

  private rejectPendingPrompt() {
    if (this._pendingPrompt) {
      this._pendingPrompt.reject();
      this._pendingPrompt = undefined;
    }
  }

  private resetOmniboxText() {
    this._text = '';
    this._omniboxSelectionIndex = 0;
  }
}
