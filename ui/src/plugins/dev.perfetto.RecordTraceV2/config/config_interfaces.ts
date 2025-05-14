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

import m from 'mithril';
import {TargetPlatformId} from '../interfaces/target_platform';
import {TraceConfigBuilder} from './trace_config_builder';
import {RecordPluginSchema, RecordSessionSchema} from '../serialization_schema';

/**
 * A sub-page of the Record page.
 * Each section maps to an entry in the left sidebar of the recording page.
 * There are three types of subpages. The last two are identical with exception
 * of the serialization scope.
 * 1. Probes pages: the ones that are a structured collection of probes that
 *    can be toggled.
 * 2,3. Session and global pages: they care of their own rendering and
 *    de/serialization.
 * 2. Session pages serialize their state in the per-session object (e.g. buffer
 *    sizes). This object can be shared with other people when using the share
 *    config feature.
 * 3. Global pages instead hold onto the "global" state of the plugin, which is
 *    not tied to the specific config of the recording session (e.g. the target
 *    being recorded, the list of saved configs). This state is retained in
 *    localstorage but is NOT shared with others.
 */
export type RecordSubpage = {
  /** A unique string. This becomes the subpage in the fragment #!/record/xxx */
  readonly id: string;

  /** The name of the material-design icon that is displayed on the sidebar. */
  readonly icon: string;

  /** The main text displayed in the left sidebar. */
  readonly title: string;

  /** The subtitle displayed when hovering over the entry of the sidebar. */
  readonly subtitle: string;
} & (
  | {
      kind: 'PROBES_PAGE';

      /** The list of probes (togglable entries) for this section. */
      readonly probes: ReadonlyArray<RecordProbe>;
    }
  | {
      kind: 'SESSION_PAGE';
      render(): m.Children;

      // Save-restore the page state into the JSON object that is saved in
      // localstorage and shared when sharing a config.
      serialize(state: RecordSessionSchema): void;
      deserialize(state: RecordSessionSchema): void;
    }
  | {
      kind: 'GLOBAL_PAGE';
      render(): m.Children;

      // Save-restore the page state into the JSON object that is saved in
      // localstorage.
      serialize(state: RecordPluginSchema): void;
      deserialize(state: RecordPluginSchema): void;
    }
);

export interface RecordProbe {
  /**
   * lower_with_under id. Keep stable, is used for serialization.
   * This id must be globally unique (not just per-section).
   */
  readonly id: string;

  /** Human readable name. */
  readonly title: string;

  /** (optional) decription. */
  readonly description?: string;

  /** (optional) file name of a .png file under assets/. */
  readonly image?: string;

  /** (optional) Link to documentation (e.g. 'https://docs.perfetto.dev/...') */
  readonly docsLink?: string;

  /** (optional). If specified restricts the probe to the given platorms. */
  readonly supportedPlatforms?: TargetPlatformId[];

  /** (optional): a list of settings for the probe (e.g. polling interval). */
  readonly settings?: Record<string, ProbeSetting>;

  /**
   * (optional): a list of probe IDs that will be force-enabled if this probe is
   * also enabled.
   */
  readonly dependencies?: string[];

  /**
   * Generate the TraceConfig for the probe. This happens in vdom-style: every
   * time we make a change to the probes the RecordingManager starts a blank
   * TraceConfigBuilder and asks all probes to update its config invoking this
   * method.
   */
  genConfig(tc: TraceConfigBuilder): void;
}

/**
 * The interface to create widgets that change the state of a probe.
 * The widget is maintains its own state and must be able to de/serialize it.
 * Realistically you don't want to implment this interface yourself but use one
 * of the pre-made widgets under ../pages/widgets/, e.g., Slider().
 */
export interface ProbeSetting {
  readonly render: () => m.Children;

  // The two methods below are supposed to save/restore the state of the setting
  // in a JSON-serializable entity (object | number | string | boolean). This is
  // to support saving configs into localstorage and sharing them.
  serialize(): unknown;
  deserialize(state: unknown): void;
}

export function supportsPlatform(
  probe: RecordProbe,
  platform: TargetPlatformId,
): boolean {
  return (
    probe.supportedPlatforms === undefined ||
    probe.supportedPlatforms.includes(platform)
  );
}
