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

import {OmniboxManager, PromptOption} from '../public/omnibox';
import {raf} from './raf_scheduler';

export enum OmniboxMode {
  Search,
  Query,
  Command,
  Prompt,
}

interface Prompt {
  text: string;
  options?: PromptOption[];
  resolve(result: string | undefined): void;
}

const defaultMode = OmniboxMode.Search;

export class OmniboxManagerImpl implements OmniboxManager {
  private _mode = defaultMode;
  private _focusOmniboxNextRender = false;
  private _pendingCursorPlacement?: number;
  private _pendingPrompt?: Prompt;
  private _omniboxSelectionIndex = 0;
  private _forceShortTextSearch = false;
  private _textForMode = new Map<OmniboxMode, string>();
  private _statusMessageContainer: {msg?: string} = {};

  get mode(): OmniboxMode {
    return this._mode;
  }

  get pendingPrompt(): Prompt | undefined {
    return this._pendingPrompt;
  }

  get text(): string {
    return this._textForMode.get(this._mode) ?? '';
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
    this._textForMode.set(this._mode, value);
  }

  setSelectionIndex(index: number): void {
    this._omniboxSelectionIndex = index;
  }

  focus(cursorPlacement?: number): void {
    this._focusOmniboxNextRender = true;
    this._pendingCursorPlacement = cursorPlacement;
    raf.scheduleFullRedraw();
  }

  clearFocusFlag(): void {
    this._focusOmniboxNextRender = false;
    this._pendingCursorPlacement = undefined;
  }

  setMode(mode: OmniboxMode, focus = true): void {
    this._mode = mode;
    this._focusOmniboxNextRender = focus;
    this._omniboxSelectionIndex = 0;
    this.rejectPendingPrompt();
    raf.scheduleFullRedraw();
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
    raf.scheduleFullRedraw();
  }

  get statusMessage(): string | undefined {
    return this._statusMessageContainer.msg;
  }

  // Start a prompt. If options are supplied, the user must pick one from the
  // list, otherwise the input is free-form text.
  prompt(text: string, options?: PromptOption[]): Promise<string | undefined> {
    this._mode = OmniboxMode.Prompt;
    this._omniboxSelectionIndex = 0;
    this.rejectPendingPrompt();

    const promise = new Promise<string | undefined>((resolve) => {
      this._pendingPrompt = {
        text,
        options,
        resolve,
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
    this.rejectPendingPrompt();
    this.setMode(OmniboxMode.Search);
  }

  reset(focus = true): void {
    this.setMode(defaultMode, focus);
    this._omniboxSelectionIndex = 0;
    this._statusMessageContainer = {};
    raf.scheduleFullRedraw();
  }

  private rejectPendingPrompt() {
    if (this._pendingPrompt) {
      this._pendingPrompt.resolve(undefined);
      this._pendingPrompt = undefined;
    }
  }
}
