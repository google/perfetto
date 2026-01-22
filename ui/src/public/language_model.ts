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
 * Public API for language model providers.
 *
 * Plugins can register language model providers using the LanguageModelManager.
 * Other plugins can then use registered providers without knowing about the
 * specific implementation plugins.
 */

/**
 * Information about a language model provider for UI display.
 */
export interface LanguageModelProviderInfo {
  /** Unique identifier for the provider */
  readonly id: string;
  /** Human-readable name for UI display */
  readonly name: string;
  /** Whether this provider requires an API key */
  readonly requiresApiKey: boolean;
  /** Optional description of the provider */
  readonly description?: string;
}

/**
 * Status of a language model provider's availability.
 */
export type LanguageModelStatus =
  | {status: 'available'}
  | {status: 'not-supported'; message: string}
  | {status: 'unavailable'; message: string}
  | {status: 'downloadable'}
  | {status: 'downloading'; progress: number};

/**
 * Options for generating text with a language model.
 */
export interface GenerateOptions {
  /** System prompt that sets the context/role for the model */
  readonly systemPrompt: string;
  /** User prompt/query to generate a response for */
  readonly userPrompt: string;
  /** Callback for streaming partial responses */
  readonly onProgress?: (partialResponse: string) => void;
}

/**
 * Interface that language model providers must implement.
 */
export interface LanguageModelProvider {
  /** Provider information for UI display */
  readonly info: LanguageModelProviderInfo;

  /**
   * Check the availability status of this provider.
   * For providers that require downloads or setup, this indicates what's needed.
   */
  checkStatus(): Promise<LanguageModelStatus>;

  /**
   * Generate text using this provider.
   * @param options Generation options including prompts and callbacks
   * @returns The generated text response
   * @throws Error if generation fails or provider is not available
   */
  generate(options: GenerateOptions): Promise<string>;

  /**
   * Download the model if required (optional).
   * Only needed for providers that require local model downloads.
   * @param onProgress Callback for download progress (0-100)
   */
  downloadModel?(onProgress: (progress: number) => void): Promise<void>;
}

/**
 * Manager for registering and accessing language model providers.
 */
export interface LanguageModelManager {
  /**
   * Register a language model provider.
   * @param provider The provider to register
   */
  registerProvider(provider: LanguageModelProvider): void;

  /**
   * Get all registered providers.
   * @returns Array of registered providers
   */
  getProviders(): LanguageModelProvider[];

  /**
   * Get a specific provider by ID.
   * @param id The provider ID
   * @returns The provider, or undefined if not found
   */
  getProvider(id: string): LanguageModelProvider | undefined;

  /**
   * Get the currently selected/active provider ID.
   * This is stored in settings and persisted across sessions.
   */
  getPreferredProviderId(): string;

  /**
   * Set the active provider by ID.
   * @param id The provider ID to set as active
   */
  setPreferredProviderId(id: string): void;

  /**
   * Get the currently active provider.
   * Returns the provider matching the active provider ID, or the first
   * available provider if the active one is not found.
   */
  getPreferredProvider(): LanguageModelProvider | undefined;
}
