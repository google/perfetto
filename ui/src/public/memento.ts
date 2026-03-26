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

/**
 * Defines the interfaces for managing application memento state.
 *
 * Mementos are persisted UI state that is not intended to be directly edited by
 * the user (unlike settings). Examples include datagrid column configurations,
 * panel sizes, and currently selected tabs.
 *
 * Mementos are stored in a separate localStorage key from settings, so users
 * can easily wipe cached UI state without affecting their settings.
 */

import {z} from 'zod';

/**
 * Describes a memento entry before it is registered with the MementoManager.
 * Unlike SettingDescriptor, this has no name/description/render fields since
 * mementos have no settings page UI.
 * @template T The type of the memento's value.
 */
export interface MementoDescriptor<T> {
  // A unique identifier for the memento. Used as the storage key.
  readonly id: string;
  // The Zod schema used for validating the memento's value.
  readonly schema: z.ZodType<T>;
  // The default value if the memento is absent from the underlying storage.
  readonly defaultValue: T;
}

/**
 * Represents a registered memento instance.
 * @template T The type of the memento's value.
 */
export interface Memento<T> extends MementoDescriptor<T>, Disposable {
  // Returns true if this memento is currently set to the default value.
  readonly isDefault: boolean;
  // Get the current value of the memento.
  get(): T;
  // Set the value of the memento. This will also update the underlying storage.
  set(value: T): void;
  // Resets back to default.
  reset(): void;
}

/**
 * Manages the registration and retrieval of application mementos.
 */
export interface MementoManager {
  /**
   * Registers a new memento.
   * @returns A handle used to interact with the memento.
   */
  register<T>(memento: MementoDescriptor<T>): Memento<T>;
  /**
   * Resets all mementos back to their default values, clearing all stored
   * UI state.
   */
  resetAll(): void;
}
