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
import {AppImpl} from '../../core/app_impl';
import {PerfettoPlugin} from '../../public/plugin';
import {RouteArgs} from '../../public/route_schema';
import {
  initializeServers,
  initializeServerFromManifest,
  loadManifest,
  showErrorsOnCompletion,
} from './extension_server';
import {
  ExtensionServer,
  Manifest,
  UserInput,
  extensionServerSchema,
  extensionServersSchema,
} from './types';
import {Setting} from '../../public/settings';
import {Button} from '../../widgets/button';
import {EmptyState} from '../../widgets/empty_state';
import {showAddExtensionServerModal} from './add_extension_server_modal';
import {
  makeDisplayUrl,
  normalizeHttpsUrl,
  sameServerLocation,
} from './url_utils';
import {copyToClipboard} from '../../base/clipboard';
import {Router} from '../../core/router';
import {
  base64Decode,
  base64Encode,
  utf8Decode,
  utf8Encode,
} from '../../base/string_utils';
import {Result} from '../../base/result';

export default class ExtensionServersPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.ExtensionServers';

  static onActivate(ctx: AppImpl, args: RouteArgs) {
    // Register the extension servers setting
    const setting = ctx.settings.register<ExtensionServer[]>({
      id: 'dev.perfetto.ExtensionServers',
      name: 'Extension Servers',
      description:
        'Configure servers that provide additional macros, SQL modules, and proto descriptors.',
      defaultValue: [],
      schema: extensionServersSchema,
      render: renderExtensionServersSettings,
    });

    // Kick off embedder extension server manifest fetch eagerly.
    const embedder = loadEmbedderExtServer(ctx);

    // These must run synchronously during onActivate (before
    // analytics.initialize()) so the dimensions are registered in time.
    if (embedder) {
      const isInternal = embedder.manifest.then((r) => r.ok);

      // TODO(lalitm): break up isInternalUser into a bunch of smaller things
      // based on where this is used.
      ctx.setIsInternalUser(isInternal);

      // TODO(lalitm): ideally this dimension *comes* from the extension server
      // or embedder code rather than being baked in here.
      ctx.analytics.addDimension(
        isInternal.then((v) => ({
          key: 'perfetto_is_internal_user',
          value: v ? '1' : '0',
        })),
      );
    }

    maybeAddExtServerFromUrl(setting, args).then(async () => {
      const nonEmbedderServers = setting
        .get()
        .filter((s) => !embedder || !sameServerLocation(s, embedder.location));
      const [embedderResults, serverResults] = await Promise.all([
        maybeAddEmbedderExtServer(ctx, setting, embedder),
        initializeServers(ctx, nonEmbedderServers),
      ]);
      showErrorsOnCompletion([...embedderResults, ...serverResults]);
    });
  }
}

// Returns a copy of the server with secret fields (e.g. PAT) removed,
// suitable for sharing via URL.
function stripSecrets(server: ExtensionServer): ExtensionServer {
  if (server.auth.type === 'none') return server;
  return {...server, auth: {type: 'none'}};
}

// Shows the add-server modal and appends the result to the setting.
// |prefill| optionally pre-populates the modal (e.g. from a shared link).
async function addServer(
  setting: Setting<ExtensionServer[]>,
  prefill?: ExtensionServer,
): Promise<ExtensionServer | undefined> {
  const result = await showAddExtensionServerModal({prefill});
  if (result) {
    setting.set([...setting.get(), result]);
  }
  return result;
}

// Shows the edit-server modal for the server at |index| and persists changes.
// |override| optionally replaces the server shown in the modal (e.g. with
// merged modules from a shared link).
async function editServer(
  setting: Setting<ExtensionServer[]>,
  index: number,
  override?: ExtensionServer,
): Promise<ExtensionServer | undefined> {
  const existing = override ?? setting.get()[index];
  const result = await showAddExtensionServerModal({existingServer: existing});
  if (result) {
    const updated = [...setting.get()];
    updated[index] = result;
    setting.set(updated);
  }
  return result;
}

function authLabel(server: ExtensionServer): string | undefined {
  if (server.auth.type === 'github_pat') return ' | Auth: PAT';
  if (server.auth.type === 'https_basic') return ' | Auth: Basic';
  if (server.auth.type === 'https_apikey') return ' | Auth: API Key';
  if (server.auth.type === 'https_sso') return ' | Auth: SSO';
  return undefined;
}

export function renderExtensionServersSettings(
  setting: Setting<ExtensionServer[]>,
): m.Children {
  const servers = setting.get();
  const shareServer = (index: number) => {
    const server = servers[index];
    const stripped = stripSecrets(server);
    const json = JSON.stringify(stripped);
    const b64 = base64Encode(utf8Encode(json));
    const pluginId = ExtensionServersPlugin.id;
    const url = `${window.location.origin}#!/?${pluginId}:addServer=${encodeURIComponent(b64)}`;
    copyToClipboard(url);
  };
  const deleteServer = (index: number) => {
    const newServers = servers.filter((_, i) => i !== index);
    setting.set(newServers);
  };
  const toggleEnabled = (index: number) => {
    const newServers = [...servers];
    newServers[index] = {
      ...newServers[index],
      enabled: !newServers[index].enabled,
    };
    setting.set(newServers);
  };
  return m(
    '.pf-extension-servers-settings',
    servers.length === 0
      ? m(
          '.pf-extension-servers-settings__empty',
          m(
            EmptyState,
            {
              title: 'No extension servers configured',
              icon: 'dns',
            },
            m(Button, {
              label: 'Add Server',
              icon: 'add',
              onclick: () => addServer(setting),
            }),
          ),
        )
      : [
          m(
            '.pf-extension-servers-settings__header',
            m(Button, {
              label: 'Add Server',
              icon: 'add',
              onclick: () => addServer(setting),
            }),
          ),
          m(
            '.pf-extension-servers-settings__list',
            // Sort embedder-managed servers to the top, preserving relative order.
            [...servers.keys()]
              .sort((a, b) => {
                const aManaged =
                  servers[a].origin === 'embedder_managed' ? 0 : 1;
                const bManaged =
                  servers[b].origin === 'embedder_managed' ? 0 : 1;
                return aManaged - bManaged || a - b;
              })
              .map((idx) => {
                const server = servers[idx];
                return m(
                  '.pf-extension-servers-settings__item',
                  {key: idx},
                  m(
                    '.pf-extension-servers-settings__item-content',
                    m(
                      '.pf-extension-servers-settings__item-url',
                      makeDisplayUrl(server),
                    ),
                    m(
                      '.pf-extension-servers-settings__item-info',
                      `${server.enabledModules.length} module${server.enabledModules.length === 1 ? '' : 's'}`,
                      authLabel(server),
                      !server.enabled && ' (Disabled)',
                    ),
                  ),
                  m(
                    '.pf-extension-servers-settings__item-actions',
                    m(Button, {
                      icon: server.enabled ? 'toggle_on' : 'toggle_off',
                      title: server.enabled ? 'Disable' : 'Enable',
                      tooltip: server.enabled ? 'Disable' : 'Enable',
                      compact: true,
                      onclick: () => toggleEnabled(idx),
                    }),
                    m(Button, {
                      icon: 'edit',
                      title: 'Edit',
                      tooltip: 'Edit',
                      compact: true,
                      onclick: () => editServer(setting, idx),
                    }),
                    server.origin !== 'embedder_managed' &&
                      m(Button, {
                        icon: 'share',
                        title: 'Share',
                        tooltip: 'Copy shareable URL',
                        compact: true,
                        onclick: () => shareServer(idx),
                      }),
                    server.origin !== 'embedder_managed' &&
                      m(Button, {
                        icon: 'delete',
                        title: 'Delete',
                        tooltip: 'Delete',
                        compact: true,
                        onclick: () => deleteServer(idx),
                      }),
                  ),
                );
              }),
          ),
        ],
  );
}

// Builds the UserInput location and kicks off the manifest fetch for the
// embedder extension server. Returns undefined if no embedder server is
// configured.
function loadEmbedderExtServer(ctx: AppImpl) {
  const extServer = ctx.embedder.extensionServer;
  if (extServer === undefined) return undefined;
  const location: UserInput = {
    type: 'https',
    url: normalizeHttpsUrl(extServer.url),
    auth:
      extServer.authType === 'https_sso' ? {type: 'https_sso'} : {type: 'none'},
  };
  return {location, manifest: loadManifest(location)};
}

// If the embedder extension server isn't already in the user's saved settings,
// persist it and initialize it. If it is already there, just initialize it.
// The manifest is pre-loaded so the fetch is shared with internal user detection.
async function maybeAddEmbedderExtServer(
  ctx: AppImpl,
  setting: Setting<ExtensionServer[]>,
  embedder?: {location: UserInput; manifest: Promise<Result<Manifest>>},
): Promise<Result<unknown>[]> {
  if (!embedder) return [];
  const {location} = embedder;
  const manifest = await embedder.manifest;
  if (!manifest.ok) return [];

  // If already configured by the user, initialize the existing entry.
  const existing = setting.get().find((s) => sameServerLocation(s, location));
  if (existing) {
    return initializeServerFromManifest(
      ctx,
      existing,
      Promise.resolve(manifest),
    );
  }

  // Otherwise, persist a new server entry and initialize it.
  const hasDefault = manifest.value.modules.some((m) => m.id === 'default');
  const server: ExtensionServer = {
    ...location,
    enabledModules: hasDefault ? ['default'] : [],
    enabled: true,
    origin: 'embedder_managed',
  };
  setting.set([...setting.get(), server]);
  return initializeServerFromManifest(ctx, server, Promise.resolve(manifest));
}

// Handles the "share extension server" flow. When a user clicks a shared URL
// like `#!/?dev.perfetto.ExtensionServers:addServer=<base64>`, this function:
//
// 1. Decodes the base64 payload into an ExtensionServer (with secrets stripped
//    by the sharer via stripSecrets()).
// 2. If a server with the same location already exists, opens the edit modal
//    with the shared link's enabled modules merged in, so the user can review.
// 3. Otherwise, opens the add-server modal pre-populated with the shared
//    server's fields (including enabledModules).
//
// In both cases the user must confirm before any changes are persisted.
async function maybeAddExtServerFromUrl(
  setting: Setting<ExtensionServer[]>,
  args: RouteArgs,
): Promise<void> {
  const encoded = args.addServer;
  if (typeof encoded !== 'string' || encoded === '') return;

  let json: string;
  try {
    json = utf8Decode(base64Decode(encoded));
  } catch {
    console.warn('ExtensionServers: failed to base64-decode addServer arg');
    return;
  }

  const parsed = extensionServerSchema.safeParse(JSON.parse(json));
  if (!parsed.success) {
    console.warn('ExtensionServers: invalid addServer payload', parsed.error);
    return;
  }
  const prefill = parsed.data;

  const servers = setting.get();
  const dupIdx = servers.findIndex((s) => sameServerLocation(s, prefill));
  if (dupIdx !== -1) {
    // Server already configured — replace its enabled modules with those from
    // the shared link and open the edit modal so the user can review.
    await editServer(setting, dupIdx, {
      ...servers[dupIdx],
      enabledModules: prefill.enabledModules,
    });
  } else {
    await addServer(setting, prefill);
  }
  // Clean the URL regardless of whether the user saved or cancelled.
  Router.navigate('#!/');
}
