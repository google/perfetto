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

import * as m from 'mithril';

import {DataSource} from '../../common/recordingV2/recording_interfaces_v2';
import {getBuiltinChromeCategoryList, isChromeTarget} from '../../common/state';
import {globals} from '../globals';
import {
  CategoriesCheckboxList,
  CompactProbe,
  Toggle,
  ToggleAttrs,
} from '../record_widgets';

import {RecordingSectionAttrs} from './recording_sections';

function extractChromeCategories(dataSources: DataSource[]): string[]|
    undefined {
  for (const dataSource of dataSources) {
    if (dataSource.name === 'chromeCategories') {
      return dataSource.descriptor as string[];
    }
  }
  return undefined;
}

class ChromeCategoriesSelection implements
    m.ClassComponent<RecordingSectionAttrs> {
  view({attrs}: m.CVnode<RecordingSectionAttrs>) {
    // If we are attempting to record via the Chrome extension, we receive the
    // list of actually supported categories via DevTools. Otherwise, we fall
    // back to an integrated list of categories from a recent version of Chrome.
    let categories = globals.state.chromeCategories ||
        extractChromeCategories(attrs.dataSources);
    if (!categories || !isChromeTarget(globals.state.recordingTarget)) {
      categories = getBuiltinChromeCategoryList();
    }

    const defaultCategories = new Map<string, string>();
    const disabledByDefaultCategories = new Map<string, string>();
    const disabledPrefix = 'disabled-by-default-';
    categories.forEach((cat) => {
      if (cat.startsWith(disabledPrefix)) {
        disabledByDefaultCategories.set(cat, cat.replace(disabledPrefix, ''));
      } else {
        defaultCategories.set(cat, cat);
      }
    });

    return m(
        '.chrome-categories',
        m(CategoriesCheckboxList, {
          categories: defaultCategories,
          title: 'Additional categories',
          get: (cfg) => cfg.chromeCategoriesSelected,
          set: (cfg, val) => cfg.chromeCategoriesSelected = val,
        }),
        m(CategoriesCheckboxList, {
          categories: disabledByDefaultCategories,
          title: 'High overhead categories',
          get: (cfg) => cfg.chromeHighOverheadCategoriesSelected,
          set: (cfg, val) => cfg.chromeHighOverheadCategoriesSelected = val,
        }));
  }
}

export class ChromeSettings implements m.ClassComponent<RecordingSectionAttrs> {
  view({attrs}: m.CVnode<RecordingSectionAttrs>) {
    return m(
        `.record-section${attrs.cssClass}`,
        CompactProbe({
          title: 'Task scheduling',
          setEnabled: (cfg, val) => cfg.taskScheduling = val,
          isEnabled: (cfg) => cfg.taskScheduling,
        }),
        CompactProbe({
          title: 'IPC flows',
          setEnabled: (cfg, val) => cfg.ipcFlows = val,
          isEnabled: (cfg) => cfg.ipcFlows,
        }),
        CompactProbe({
          title: 'Javascript execution',
          setEnabled: (cfg, val) => cfg.jsExecution = val,
          isEnabled: (cfg) => cfg.jsExecution,
        }),
        CompactProbe({
          title: 'Web content rendering, layout and compositing',
          setEnabled: (cfg, val) => cfg.webContentRendering = val,
          isEnabled: (cfg) => cfg.webContentRendering,
        }),
        CompactProbe({
          title: 'UI rendering & surface compositing',
          setEnabled: (cfg, val) => cfg.uiRendering = val,
          isEnabled: (cfg) => cfg.uiRendering,
        }),
        CompactProbe({
          title: 'Input events',
          setEnabled: (cfg, val) => cfg.inputEvents = val,
          isEnabled: (cfg) => cfg.inputEvents,
        }),
        CompactProbe({
          title: 'Navigation & Loading',
          setEnabled: (cfg, val) => cfg.navigationAndLoading = val,
          isEnabled: (cfg) => cfg.navigationAndLoading,
        }),
        CompactProbe({
          title: 'Chrome Logs',
          setEnabled: (cfg, val) => cfg.chromeLogs = val,
          isEnabled: (cfg) => cfg.chromeLogs,
        }),
        m(Toggle, {
          title: 'Remove untyped and sensitive data like URLs from the trace',
          descr: 'Not recommended unless you intend to share the trace' +
              ' with third-parties.',
          setEnabled: (cfg, val) => cfg.chromePrivacyFiltering = val,
          isEnabled: (cfg) => cfg.chromePrivacyFiltering,
        } as ToggleAttrs),
        m(ChromeCategoriesSelection, attrs));
  }
}
