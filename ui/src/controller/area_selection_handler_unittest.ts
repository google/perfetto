// Copyright (C) 2021 The Android Open Source Project
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

import {createEmptyState} from '../common/empty_state';
import {AreaById} from '../common/state';
import {globals} from '../frontend/globals';

import {AreaSelectionHandler} from './area_selection_handler';

test('validAreaAfterUndefinedArea', () => {
  const areaId = '0';
  const latestArea: AreaById = {startSec: 0, endSec: 1, tracks: [], id: areaId};
  globals.state = createEmptyState();
  globals.state.currentSelection = {kind: 'AREA', areaId};
  globals.state.areas[areaId] = latestArea;

  const areaSelectionHandler = new AreaSelectionHandler();
  const [hasAreaChanged, selectedArea] = areaSelectionHandler.getAreaChange();

  expect(hasAreaChanged).toEqual(true);
  expect(selectedArea).toEqual(latestArea);
});

test('UndefinedAreaAfterValidArea', () => {
  const previousAreaId = '0';
  const previous:
      AreaById = {startSec: 0, endSec: 1, tracks: [], id: previousAreaId};
  globals.state = createEmptyState();
  globals.state.currentSelection = {
    kind: 'AREA',
    areaId: previousAreaId,
  };
  globals.state.areas[previousAreaId] = previous;
  const areaSelectionHandler = new AreaSelectionHandler();
  areaSelectionHandler.getAreaChange();

  const currentAreaId = '1';
  globals.state.currentSelection = {
    kind: 'AREA',
    areaId: currentAreaId,
  };
  const [hasAreaChanged, selectedArea] = areaSelectionHandler.getAreaChange();

  expect(hasAreaChanged).toEqual(true);
  expect(selectedArea).toEqual(undefined);
});

test('UndefinedAreaAfterUndefinedArea', () => {
  globals.state.currentSelection = {kind: 'AREA', areaId: '0'};
  const areaSelectionHandler = new AreaSelectionHandler();
  areaSelectionHandler.getAreaChange();

  globals.state.currentSelection = {kind: 'AREA', areaId: '1'};
  const [hasAreaChanged, selectedArea] = areaSelectionHandler.getAreaChange();

  expect(hasAreaChanged).toEqual(true);
  expect(selectedArea).toEqual(undefined);
});

test('validAreaAfterValidArea', () => {
  const previousAreaId = '0';
  const previous:
      AreaById = {startSec: 0, endSec: 1, tracks: [], id: previousAreaId};
  globals.state = createEmptyState();
  globals.state.currentSelection = {
    kind: 'AREA',
    areaId: previousAreaId,
  };
  globals.state.areas[previousAreaId] = previous;
  const areaSelectionHandler = new AreaSelectionHandler();
  areaSelectionHandler.getAreaChange();

  const currentAreaId = '1';
  const current:
      AreaById = {startSec: 1, endSec: 2, tracks: [], id: currentAreaId};
  globals.state.currentSelection = {
    kind: 'AREA',
    areaId: currentAreaId,
  };
  globals.state.areas[currentAreaId] = current;
  const [hasAreaChanged, selectedArea] = areaSelectionHandler.getAreaChange();

  expect(hasAreaChanged).toEqual(true);
  expect(selectedArea).toEqual(current);
});

test('sameAreaSelected', () => {
  const previousAreaId = '0';
  const previous:
      AreaById = {startSec: 0, endSec: 1, tracks: [], id: previousAreaId};
  globals.state = createEmptyState();
  globals.state.currentSelection = {
    kind: 'AREA',
    areaId: previousAreaId,
  };
  globals.state.areas[previousAreaId] = previous;
  const areaSelectionHandler = new AreaSelectionHandler();
  areaSelectionHandler.getAreaChange();

  const currentAreaId = '0';
  const current:
      AreaById = {startSec: 0, endSec: 1, tracks: [], id: currentAreaId};
  globals.state.currentSelection = {
    kind: 'AREA',
    areaId: currentAreaId,
  };
  globals.state.areas[currentAreaId] = current;
  const [hasAreaChanged, selectedArea] = areaSelectionHandler.getAreaChange();

  expect(hasAreaChanged).toEqual(false);
  expect(selectedArea).toEqual(current);
});

test('NonAreaSelectionAfterUndefinedArea', () => {
  globals.state.currentSelection = {kind: 'AREA', areaId: '0'};
  const areaSelectionHandler = new AreaSelectionHandler();
  areaSelectionHandler.getAreaChange();

  globals.state
      .currentSelection = {kind: 'COUNTER', leftTs: 0, rightTs: 0, id: 1};
  const [hasAreaChanged, selectedArea] = areaSelectionHandler.getAreaChange();

  expect(hasAreaChanged).toEqual(false);
  expect(selectedArea).toEqual(undefined);
});
