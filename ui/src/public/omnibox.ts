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
   * Turns the omnibox into an interactive prompt for the user. Think of
   * window.prompt() but non-modal and more integrated with the UI.
   *
   * This method has multiple overloads for different use cases:
   *
   * 1. **Free-form text input**: User can type any string
   *    - `prompt(text: string): Promise<string | undefined>`
   *    - `prompt(text: string, defaultValue: string): Promise<string | undefined>`
   *
   * 2. **Choice selection**: User picks from a predefined list
   *    - `prompt(text: string, choices: ReadonlyArray<string>): Promise<string | undefined>`
   *    - `prompt<T>(text: string, choices: PromptChoices<T>): Promise<T | undefined>`
   *
   * @param text - The question shown to the user (e.g. "Select a process to
   *   jump to").
   * @returns The user's input (string for free-form) or selected choice
   *   (string/T for choices). Returns `undefined` if the user dismisses the
   *   prompt by pressing Esc or clicking elsewhere.
   *
   * Optional parameters:
   *
   * defaultValue - For free-form input: optional default value pre-filled in
   *   the input field.
   * choices For choice selection: either a simple array of strings or
   *   a PromptChoices object for complex data.
   *
   * @example
   * ```ts
   * // Free-form text input
   * const name = await omnibox.prompt('Enter your name');
   *
   * // Free-form with default value
   * const name = await omnibox.prompt('Enter your name', 'John Doe');
   *
   * // Simple choice selection
   * const color = await omnibox.prompt('Choose a color...', ['red', 'blue', 'green']);
   *
   * // Complex choice objects
   * const process = await omnibox.prompt('Choose a process...', {
   *   values: [
   *     {pid: 123, name: 'system_server'},
   *     {pid: 456, name: 'com.example.app'},
   *   ],
   *   getName: (p) => `${p.name} (PID: ${p.pid})`,
   * });
   * ```
   */
  prompt(text: string): Promise<string | undefined>;
  prompt(text: string, defaultValue: string): Promise<string | undefined>;
  prompt(
    text: string,
    choices: ReadonlyArray<string>,
  ): Promise<string | undefined>;
  prompt<T>(text: string, choices: PromptChoices<T>): Promise<T | undefined>;
}

export interface PromptChoices<T> {
  values: ReadonlyArray<T>;
  getName: (x: T) => string;
}
