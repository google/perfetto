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

import {Time} from '../base/time';
import {createEmptyState} from '../common/empty_state';
import {AreaById} from '../common/state';
import {globals} from '../frontend/globals';

import {AreaSelectionHandler} from './area_selection_handler';

beforeAll(() => {
  // Ensure store exists and reset to a known state.
  globals.initStore(createEmptyState());
});

test('validAreaAfterUndefinedArea', () => {
  const areaId = '0';
  const latestArea: AreaById =
      {start: Time.fromRaw(0n), end: Time.fromRaw(1n), tracks: [], id: areaId};
  globals.store.edit((draft) => {
    draft.currentSelection = {kind: 'AREA', areaId: areaId};
    draft.areas[areaId] = latestArea;
  });

  const areaSelectionHandler = new AreaSelectionHandler();
  const [hasAreaChanged, selectedArea] = areaSelectionHandler.getAreaChange();

  expect(hasAreaChanged).toEqual(true);
  expect(selectedArea).toEqual(latestArea);
});

test('UndefinedAreaAfterValidArea', () => {
  const previousAreaId = '0';
  const previous: AreaById = {
    start: Time.fromRaw(0n),
    end: Time.fromRaw(1n),
    tracks: [],
    id: previousAreaId,
  };
  globals.store.edit((draft) => {
    draft.currentSelection = {
      kind: 'AREA',
      areaId: previousAreaId,
    };
    draft.areas[previousAreaId] = previous;
  });
  const areaSelectionHandler = new AreaSelectionHandler();
  areaSelectionHandler.getAreaChange();

  const currentAreaId = '1';
  globals.store.edit((draft) => {
    draft.currentSelection = {
      kind: 'AREA',
      areaId: currentAreaId,
    };
  });
  const [hasAreaChanged, selectedArea] = areaSelectionHandler.getAreaChange();

  expect(hasAreaChanged).toEqual(true);
  expect(selectedArea).toEqual(undefined);
});

test('UndefinedAreaAfterUndefinedArea', () => {
  globals.store.edit((draft) => {
    draft.currentSelection = {kind: 'AREA', areaId: '0'};
  });
  const areaSelectionHandler = new AreaSelectionHandler();
  areaSelectionHandler.getAreaChange();

  globals.store.edit((draft) => {
    draft.currentSelection = {kind: 'AREA', areaId: '1'};
  });
  const [hasAreaChanged, selectedArea] = areaSelectionHandler.getAreaChange();

  expect(hasAreaChanged).toEqual(true);
  expect(selectedArea).toEqual(undefined);
});

test('validAreaAfterValidArea', () => {
  const previousAreaId = '0';
  const previous: AreaById = {
    start: Time.fromRaw(0n),
    end: Time.fromRaw(1n),
    tracks: [],
    id: previousAreaId,
  };
  globals.store.edit((draft) => {
    draft.currentSelection = {
      kind: 'AREA',
      areaId: previousAreaId,
    };
    draft.areas[previousAreaId] = previous;
  });
  const areaSelectionHandler = new AreaSelectionHandler();
  areaSelectionHandler.getAreaChange();

  const currentAreaId = '1';
  const current: AreaById = {
    start: Time.fromRaw(1n),
    end: Time.fromRaw(2n),
    tracks: [],
    id: currentAreaId,
  };
  globals.store.edit((draft) => {
    draft.currentSelection = {
      kind: 'AREA',
      areaId: currentAreaId,
    };
    draft.areas[currentAreaId] = current;
  });
  const [hasAreaChanged, selectedArea] = areaSelectionHandler.getAreaChange();

  expect(hasAreaChanged).toEqual(true);
  expect(selectedArea).toEqual(current);
});

test('sameAreaSelected', () => {
  const previousAreaId = '0';
  const previous: AreaById = {
    start: Time.fromRaw(0n),
    end: Time.fromRaw(1n),
    tracks: [],
    id: previousAreaId,
  };
  globals.store.edit((draft) => {
    draft.currentSelection = {
      kind: 'AREA',
      areaId: previousAreaId,
    };
    draft.areas[previousAreaId] = previous;
  });
  const areaSelectionHandler = new AreaSelectionHandler();
  areaSelectionHandler.getAreaChange();

  const currentAreaId = '0';
  const current: AreaById = {
    start: Time.fromRaw(0n),
    end: Time.fromRaw(1n),
    tracks: [],
    id: currentAreaId,
  };
  globals.store.edit((draft) => {
    draft.currentSelection = {
      kind: 'AREA',
      areaId: currentAreaId,
    };
    draft.areas[currentAreaId] = current;
  });
  const [hasAreaChanged, selectedArea] = areaSelectionHandler.getAreaChange();

  expect(hasAreaChanged).toEqual(false);
  expect(selectedArea).toEqual(current);
});

test('NonAreaSelectionAfterUndefinedArea', () => {
  globals.store.edit((draft) => {
    draft.currentSelection = {kind: 'AREA', areaId: '0'};
  });
  const areaSelectionHandler = new AreaSelectionHandler();
  areaSelectionHandler.getAreaChange();

  globals.store.edit((draft) => {
    draft.currentSelection = {
      kind: 'COUNTER',
      leftTs: Time.fromRaw(0n),
      rightTs: Time.fromRaw(0n),
      id: 1,
    };
  });
  const [hasAreaChanged, selectedArea] = areaSelectionHandler.getAreaChange();

  expect(hasAreaChanged).toEqual(false);
  expect(selectedArea).toEqual(undefined);
});
