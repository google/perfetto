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
   * @param text - The question showed to the user (e.g. "Select a process to
   * jump to").
   * @param choices - If defined, it shows a list of options in a select-box
   * fashion, where the user can move with Up/Down arrows. If omitted the input
   * is free-form, like in the case of window.prompt().
   * @returns If `options` === undefined, returns the free-form user input. If
   * `options` was provided, returns the selected choice. Returns undefined if
   * the user dismisses the prompt by pressing Esc or clicking elsewhere.
   *
   * Example:
   * ```ts
   * // Free-form string
   * const name = await prompt('Enter your name');
   *
   * // Simple list of choices
   * const value = await prompt('Choose a color...', ['red', 'blue', 'green']);
   *
   * // Each choice is an object
   * const value = await prompt('Choose from an enum...', {
   *   values: [
   *     {x: MyEnum.Foo, name: 'foo'},
   *     {x: MyEnum.Bar, name: 'bar'},
   *   ],
   *   getName: (e) => e.name,
   * );
   * ```
   */
  prompt(
    text: string,
    choices?: ReadonlyArray<string>,
  ): Promise<string | undefined>;
  prompt<T>(text: string, choices: PromptChoices<T>): Promise<T | undefined>;
}

export interface PromptChoices<T> {
  values: ReadonlyArray<T>;
  getName: (x: T) => string;
}
