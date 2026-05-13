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

// Describes a custom omnibox mode that plugins can register.
// When the user types the trigger character in search mode, the omnibox
// switches to this mode. The mode controls placeholder text, styling, and
// receives callbacks for user interactions.
export interface OmniboxModeDescriptor {
  // Single character that activates this mode from search mode (e.g. ':').
  readonly trigger: string;

  // Short hint shown in the search placeholder (e.g. "':' for SQL query").
  // If provided, appended to the default search placeholder text.
  readonly hint?: string;

  // Placeholder text shown in the input when the mode is active.
  readonly placeholder: string;

  // Optional CSS class added to the omnibox container.
  readonly className?: string;

  // Called when the user presses Enter.
  // |text| is the current omnibox text.
  // |alt| is true when Alt+Enter (or Cmd+Enter on Mac) was pressed.
  onSubmit(text: string, alt: boolean): void;

  // Called when the omnibox text changes. If not provided, the default
  // behavior is to update the omnibox text.
  onInput?(text: string): void;

  // Called when the user dismisses the omnibox (Escape or blur).
  // If not provided, the omnibox resets to search mode.
  onClose?(): void;

  // Called when the user presses Backspace on an empty input.
  // If not provided, the omnibox resets to search mode.
  onGoBack?(): void;
}

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

  // Registers a custom omnibox mode. When the user types the trigger
  // character in search mode, the omnibox switches to this mode.
  // Returns a Disposable that unregisters the mode when disposed.
  registerMode(desc: OmniboxModeDescriptor): Disposable;

  // Programmatically activates a registered mode, optionally focusing the
  // omnibox input.
  activateRegisteredMode(trigger: string, focus?: boolean): void;
}

export interface PromptChoices<T> {
  values: ReadonlyArray<T>;
  getName: (x: T) => string;
}
