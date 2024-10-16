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

export interface OmniboxManager {
  /**
   * Turns the omnibox into an interfactive prompt for the user. Think of
   * window.prompt() but non-modal and more integrated with the UI.
   * @param text The question showed to the user (e.g.
   * "Select a process to jump to")
   * @param options If defined, it shows a list of options in a select-box
   * fashion, where the user can move with Up/Down arrows. If omitted the
   * input is freeform, like in the case of window.prompt().
   * @returns the free-form user input, if `options` === undefined; returns
   * the chosen PromptOption.key if `options` was provided; returns undefined
   * if the user dimisses the prompt by pressing Esc or clicking eslewhere.
   */
  prompt(text: string, options?: PromptOption[]): Promise<string | undefined>;
}

export interface PromptOption {
  key: string;
  displayName: string;
}
