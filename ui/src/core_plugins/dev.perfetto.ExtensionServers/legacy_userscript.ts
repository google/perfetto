// Copyright (C) 2026 The Android Open Source Project
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

import m from 'mithril';
import {defer} from '../../base/deferred';
import type {AppImpl} from '../../core/app_impl';
import type {CommandInvocation} from '../../core/command_manager';
import {raf} from '../../core/raf_scheduler';
import type {SqlPackage} from '../../public/extra_sql_packages';
import {Anchor} from '../../widgets/anchor';
import {showModal} from '../../widgets/modal';

const SCRIPT_LOAD_TIMEOUT_MS = 5000;
const SCRIPT_URL =
  'https://storage.cloud.google.com/perfetto-ui-internal/internal-data-v1/amalgamated.js';

// Shape exposed on `window.globals` for the legacy amalgamated.js userscript
// to populate. The fields are append-only — renaming or removing them would
// break the externally-hosted script.
interface Globals {
  // WARNING: do not change/rename/move any of the fields below without
  // considering impact on the internal_user script.
  isInternalUser: boolean;
  readonly extraSqlPackages: SqlPackage[];
  readonly extraParsingDescriptors: string[];
  readonly extraMacros: Record<string, ReadonlyArray<CommandInvocation>>[];
  shutdown(): void;
}

declare global {
  interface Window {
    globals?: Globals;
  }
}

// Kicks off the legacy userscript fetch and returns a promise that resolves
// once the script finishes loading (or after a timeout). The resolved value
// is the populated `window.globals` object.
export function loadLegacyUserscript(): Promise<Globals> {
  const globals: Globals = {
    isInternalUser: false,
    extraSqlPackages: [],
    extraParsingDescriptors: [],
    extraMacros: [],
    shutdown() {
      raf.shutdown();
    },
  };
  window.globals = globals;

  const scriptLoaded = defer<Globals>();
  const script = document.createElement('script');
  script.src = SCRIPT_URL;
  script.async = true;
  script.onerror = () => scriptLoaded.resolve(globals);
  script.onload = () => scriptLoaded.resolve(globals);
  document.head.append(script);

  setTimeout(() => scriptLoaded.resolve(globals), SCRIPT_LOAD_TIMEOUT_MS);
  return scriptLoaded;
}

// Backcompat shim for the deprecated userscript-driven macro pipeline. The
// extension server is now the only path that registers real commands; for
// any macro that the legacy script tried to publish we either:
//   - suppress it, when the same name (after stripping a leading "[Foo] "
//     bracketed prefix) is already provided by an extension server macro;
//   - register a stub that, when invoked, tells the user to enable the
//     module on the new extension server.
//
// Must be called only after the extension server macros have settled — they
// are read via `app.macros()` to compute the suppression set.
export async function registerLegacyMacroStubs(
  app: AppImpl,
  globals: Globals,
): Promise<void> {
  const otherMacros = await app.macros();
  const extensionServerNames = new Set(
    otherMacros.filter((m) => m.source !== undefined).map((m) => m.name),
  );

  for (const record of globals.extraMacros) {
    for (const name of Object.keys(record)) {
      if (extensionServerNames.has(stripBracketedPrefix(name))) {
        continue;
      }
      app.commands.registerCommand({
        id: `dev.perfetto.UserMacro.${name}`,
        name,
        callback: () => showLegacyMacroDeprecatedModal(name),
      });
    }
  }
}

// Strips a leading "[Foo] " bracketed prefix so legacy macro names line up
// with the unprefixed names that extension server macros expose.
function stripBracketedPrefix(name: string): string {
  return name.replace(/^\s*\[[^\]]*\]\s*/, '');
}

function showLegacyMacroDeprecatedModal(name: string): void {
  showModal({
    title: 'This command has moved',
    content: m(
      'div',
      m(
        'p',
        `"${name}" is no longer enabled for all Googlers to allow the macro `,
        `to scale to more users and teams.`,
      ),
      m(
        'p',
        'Open Settings → Extension Servers and enable the module for ',
        'your team to make these commands available again.',
      ),
      m(
        'p',
        m(
          Anchor,
          {
            href: 'http://go/perfetto-ui-macro-migration',
            target: '_blank',
          },
          'See this page for more information about this migration',
        ),
      ),
    ),
    buttons: [{text: 'OK', primary: true}],
  });
}
