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

import {Action} from '../common/actions';
import {dingus} from '../test/dingus';

import {globals} from './globals';
import {MithrilEvent, quietDispatch, quietHandler} from './mithril_helpers';

// TODO(hjd): Do this in jsdom environment.
beforeEach(() => {
  globals.resetForTesting();
});

test('quietHandler', () => {
  const e = new Event('an_event') as MithrilEvent;
  e.redraw = true;
  const handler = dingus<(e: Event) => void>('handler');
  quietHandler(handler)(e);
  expect(handler.calls[0][1][0]).toBe(e);
});

test('quietDispatch with object', () => {
  const e = new Event('an_event') as MithrilEvent;
  e.redraw = true;
  const d = dingus<(action: Action) => void>('dispatch');
  globals.dispatch = d;
  const action = {};
  quietDispatch(action)(e);
  expect(e.redraw).toBe(false);
  expect(d.calls[0][1][0]).toBe(action);
});

test('quietDispatch with function', () => {
  const e = new Event('an_event') as MithrilEvent;
  e.redraw = true;

  const dispatch = dingus<(action: Action) => void>('dispatch');
  globals.dispatch = dispatch;

  const theAction = {};

  const action = (theEvent: Event) => {
    expect(theEvent).toBe(e);
    return theAction;
  };

  quietDispatch(action)(e);
  expect(e.redraw).toBe(false);
  expect(dispatch.calls[0][1][0]).toBe(theAction);
});
