// Copyright (C) 2018 The Android Open Source Project
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

import {AppImpl} from '../core/app_impl';
import {CommandInvocation} from '../core/command_manager';
import {raf} from '../core/raf_scheduler';
import {SqlPackage} from '../public/extra_sql_packages';

// This controls how long we wait for the script to load before giving up and
// proceeding as if the user is not internal.
const SCRIPT_LOAD_TIMEOUT_MS = 5000;
const SCRIPT_URL =
  'https://storage.cloud.google.com/perfetto-ui-internal/internal-data-v1/amalgamated.js';

// This interface describes the required interface that the script expects to
// find on window.globals.
interface Globals {
  // This variable is set by the is_internal_user.js script if the user is a
  // googler. This is used to avoid exposing features that are not ready yet for
  // public consumption. The gated features themselves are not secret. If a user
  // has been detected as a Googler once, make that sticky in localStorage, so
  // that we keep treating them as such when they connect over public networks.
  // WARNING: do not change/rename/move without considering impact on the
  // internal_user script.
  isInternalUser: boolean;

  // The script adds to this list, hence why it's readonly.
  // WARNING: do not change/rename/move without considering impact on the
  // internal_user script.
  readonly extraSqlPackages: SqlPackage[];

  // JSON Amalgamator populates this with statsd atom descriptors
  // as a list of base64-encoded strings.
  // WARNING: do not change/rename/move without considering impact on the
  // internal_user script.
  readonly extraParsingDescriptors: string[];

  // Command macros. The key is the macro name, value is a list of commands to
  // invoke. We use Record instead of Map because this is populated by a script
  // that writes plain JS objects.
  // WARNING: do not change/rename/move without considering impact on the
  // internal_user script.
  readonly extraMacros: Record<string, ReadonlyArray<CommandInvocation>>[];

  // Used when switching to the legacy TraceViewer UI.
  // Most resources are cleaned up by replacing the current |window| object,
  // however pending RAFs and workers seem to outlive the |window| and need to
  // be cleaned up explicitly.
  shutdown(): void;
}

// Extend the Window interface to include our globals (used below).
declare global {
  interface Window {
    globals?: Globals;
  }
}

/**
 * Sets up an object on window.globals for the internal user script to populate.
 */
function setupGlobalsProxy(): Globals {
  const params: Globals = {
    isInternalUser: false,
    extraSqlPackages: [],
    extraParsingDescriptors: [],
    extraMacros: [],
    shutdown() {
      raf.shutdown();
    },
  };
  window.globals = params;
  return params;
}

/**
 * Loads a script that detects if the user is internal, allowing access to
 * non-public features and SQL packages. Registers the loaded data directly
 * on the app instance.
 */
export async function tryLoadIsInternalUserScript(app: AppImpl): Promise<void> {
  // Set up the global object and attach it to `window` before loading the
  // script.
  const params = setupGlobalsProxy();

  await new Promise<void>((resolve) => {
    const script = document.createElement('script');
    script.src = SCRIPT_URL;
    script.async = true;
    script.onerror = () => resolve();
    script.onload = () => resolve();
    document.head.append(script);

    // Set a timeout to avoid blocking the UI for too long if the script is slow
    // to load.
    setTimeout(() => resolve(), SCRIPT_LOAD_TIMEOUT_MS);
  });

  // The script has loaded (or timed out). The params object has been mutated
  // by the script if possible. Register everything on the app.
  app.isInternalUser = params.isInternalUser;
  app.addMacros(Object.assign({}, ...params.extraMacros));
  app.addProtoDescriptors(params.extraParsingDescriptors);
  app.addSqlPackages(params.extraSqlPackages);
}
