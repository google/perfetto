// Copyright (C) 2022 The Android Open Source Project
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

import {produce} from 'immer';
import m from 'mithril';
import {Actions} from '../../common/actions';
import {DataSource} from '../../common/recordingV2/recording_interfaces_v2';
import {getBuiltinChromeCategoryList, isChromeTarget} from '../../common/state';
import {
  MultiSelect,
  MultiSelectDiff,
  Option as MultiSelectOption,
} from '../../widgets/multiselect';
import {Section} from '../../widgets/section';
import {globals} from '../globals';
import {
  CategoryGetter,
  CompactProbe,
  Toggle,
  ToggleAttrs,
} from '../record_widgets';
import {RecordingSectionAttrs} from './recording_sections';

function extractChromeCategories(
  dataSources: DataSource[],
): string[] | undefined {
  for (const dataSource of dataSources) {
    if (dataSource.name === 'chromeCategories') {
      return dataSource.descriptor as string[];
    }
  }
  return undefined;
}

class ChromeCategoriesSelection
  implements m.ClassComponent<RecordingSectionAttrs>
{
  private defaultCategoryOptions: MultiSelectOption[] | undefined = undefined;
  private disabledByDefaultCategoryOptions: MultiSelectOption[] | undefined =
    undefined;

  static updateValue(attrs: CategoryGetter, diffs: MultiSelectDiff[]) {
    const traceCfg = produce(globals.state.recordConfig, (draft) => {
      const values = attrs.get(draft);
      for (const diff of diffs) {
        const value = diff.id;
        const index = values.indexOf(value);
        const enabled = diff.checked;
        if (enabled && index === -1) {
          values.push(value);
        }
        if (!enabled && index !== -1) {
          values.splice(index, 1);
        }
      }
    });
    globals.dispatch(Actions.setRecordConfig({config: traceCfg}));
  }

  view({attrs}: m.CVnode<RecordingSectionAttrs>) {
    const categoryConfigGetter: CategoryGetter = {
      get: (cfg) => cfg.chromeCategoriesSelected,
      set: (cfg, val) => (cfg.chromeCategoriesSelected = val),
    };

    if (
      this.defaultCategoryOptions === undefined ||
      this.disabledByDefaultCategoryOptions === undefined
    ) {
      // If we are attempting to record via the Chrome extension, we receive the
      // list of actually supported categories via DevTools. Otherwise, we fall
      // back to an integrated list of categories from a recent version of
      // Chrome.
      const enabled = new Set(
        categoryConfigGetter.get(globals.state.recordConfig),
      );
      let categories =
        globals.state.chromeCategories ||
        extractChromeCategories(attrs.dataSources);
      if (!categories || !isChromeTarget(globals.state.recordingTarget)) {
        categories = getBuiltinChromeCategoryList();
      }
      this.defaultCategoryOptions = [];
      this.disabledByDefaultCategoryOptions = [];
      const disabledPrefix = 'disabled-by-default-';
      categories.forEach((cat) => {
        const checked = enabled.has(cat);

        if (
          cat.startsWith(disabledPrefix) &&
          this.disabledByDefaultCategoryOptions !== undefined
        ) {
          this.disabledByDefaultCategoryOptions.push({
            id: cat,
            name: cat.replace(disabledPrefix, ''),
            checked: checked,
          });
        } else if (
          !cat.startsWith(disabledPrefix) &&
          this.defaultCategoryOptions !== undefined
        ) {
          this.defaultCategoryOptions.push({
            id: cat,
            name: cat,
            checked: checked,
          });
        }
      });
    }

    return m(
      'div.chrome-categories',
      m(
        Section,
        {title: 'Additional Categories'},
        m(MultiSelect, {
          options: this.defaultCategoryOptions,
          repeatCheckedItemsAtTop: false,
          fixedSize: false,
          onChange: (diffs: MultiSelectDiff[]) => {
            diffs.forEach(({id, checked}) => {
              if (this.defaultCategoryOptions === undefined) {
                return;
              }
              for (const option of this.defaultCategoryOptions) {
                if (option.id == id) {
                  option.checked = checked;
                }
              }
            });
            ChromeCategoriesSelection.updateValue(categoryConfigGetter, diffs);
          },
        }),
      ),
      m(
        Section,
        {title: 'High Overhead Categories'},
        m(MultiSelect, {
          options: this.disabledByDefaultCategoryOptions,
          repeatCheckedItemsAtTop: false,
          fixedSize: false,
          onChange: (diffs: MultiSelectDiff[]) => {
            diffs.forEach(({id, checked}) => {
              if (this.disabledByDefaultCategoryOptions === undefined) {
                return;
              }
              for (const option of this.disabledByDefaultCategoryOptions) {
                if (option.id == id) {
                  option.checked = checked;
                }
              }
            });
            ChromeCategoriesSelection.updateValue(categoryConfigGetter, diffs);
          },
        }),
      ),
    );
  }
}

export class ChromeSettings implements m.ClassComponent<RecordingSectionAttrs> {
  view({attrs}: m.CVnode<RecordingSectionAttrs>) {
    return m(
      `.record-section${attrs.cssClass}`,
      CompactProbe({
        title: 'Task scheduling',
        setEnabled: (cfg, val) => (cfg.taskScheduling = val),
        isEnabled: (cfg) => cfg.taskScheduling,
      }),
      CompactProbe({
        title: 'IPC flows',
        setEnabled: (cfg, val) => (cfg.ipcFlows = val),
        isEnabled: (cfg) => cfg.ipcFlows,
      }),
      CompactProbe({
        title: 'Javascript execution',
        setEnabled: (cfg, val) => (cfg.jsExecution = val),
        isEnabled: (cfg) => cfg.jsExecution,
      }),
      CompactProbe({
        title: 'Web content rendering, layout and compositing',
        setEnabled: (cfg, val) => (cfg.webContentRendering = val),
        isEnabled: (cfg) => cfg.webContentRendering,
      }),
      CompactProbe({
        title: 'UI rendering & surface compositing',
        setEnabled: (cfg, val) => (cfg.uiRendering = val),
        isEnabled: (cfg) => cfg.uiRendering,
      }),
      CompactProbe({
        title: 'Input events',
        setEnabled: (cfg, val) => (cfg.inputEvents = val),
        isEnabled: (cfg) => cfg.inputEvents,
      }),
      CompactProbe({
        title: 'Navigation & Loading',
        setEnabled: (cfg, val) => (cfg.navigationAndLoading = val),
        isEnabled: (cfg) => cfg.navigationAndLoading,
      }),
      CompactProbe({
        title: 'Chrome Logs',
        setEnabled: (cfg, val) => (cfg.chromeLogs = val),
        isEnabled: (cfg) => cfg.chromeLogs,
      }),
      CompactProbe({
        title: 'Audio',
        setEnabled: (cfg, val) => (cfg.audio = val),
        isEnabled: (cfg) => cfg.audio,
      }),
      CompactProbe({
        title: 'Video',
        setEnabled: (cfg, val) => (cfg.video = val),
        isEnabled: (cfg) => cfg.video,
      }),
      m(Toggle, {
        title: 'Remove untyped and sensitive data like URLs from the trace',
        descr:
          'Not recommended unless you intend to share the trace' +
          ' with third-parties.',
        setEnabled: (cfg, val) => (cfg.chromePrivacyFiltering = val),
        isEnabled: (cfg) => cfg.chromePrivacyFiltering,
      } as ToggleAttrs),
      m(ChromeCategoriesSelection, attrs),
    );
  }
}
