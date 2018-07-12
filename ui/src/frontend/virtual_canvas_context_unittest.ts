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
import {ChildVirtualContext} from './child_virtual_context';
import Mock = jest.Mock;
import {RootVirtualContext} from './root_virtual_context';

const setupCanvasContext = () => {

  const ctxMock = jest.fn<RootVirtualContext>(() => ({
                                                stroke: jest.fn(),
                                                beginPath: jest.fn(),
                                                closePath: jest.fn(),
                                                measureText: jest.fn(),
                                                fillRect: jest.fn(),
                                                fillText: jest.fn(),
                                                moveTo: jest.fn(),
                                                lineTo: jest.fn(),
                                                checkRectOnCanvas: () => true,
                                              }));

  return new ctxMock();
};

test('virtual canvas context offsets work on fillrect', async () => {

  const ctx = setupCanvasContext();
  const virtualContext =
      new ChildVirtualContext(ctx, {x: 100, y: 200, width: 200, height: 150});
  const mockCalls = (ctx.fillRect as Mock).mock.calls;

  virtualContext.fillRect(10, 5, 100, 20);

  expect(mockCalls[0]).toEqual([110, 205, 100, 20]);
});

test('virtual canvas context offsets work on filltext', async () => {

  const ctx = setupCanvasContext();
  const virtualContext =
      new ChildVirtualContext(ctx, {x: 100, y: 200, width: 200, height: 150});
  const mockCalls = (ctx.fillText as Mock).mock.calls;

  virtualContext.fillText('', 10, 5);

  mockCalls[0].shift();
  expect(mockCalls[0]).toEqual([110, 205]);
});

test('virtual canvas context offsets work on moveto and lineto', async () => {

  const ctx = setupCanvasContext();
  const virtualContext =
      new ChildVirtualContext(ctx, {x: 100, y: 200, width: 200, height: 150});

  const mockCallsMove = (ctx.moveTo as Mock).mock.calls;
  virtualContext.moveTo(10, 5);
  expect(mockCallsMove[0]).toEqual([110, 205]);

  const mockCallsLine = (ctx.lineTo as Mock).mock.calls;
  virtualContext.lineTo(10, 5);
  expect(mockCallsLine[0]).toEqual([110, 205]);
});

test('virtual canvas context limits the bbox', async () => {

  const ctx = setupCanvasContext();
  const virtualContext =
      new ChildVirtualContext(ctx, {x: 100, y: 200, width: 200, height: 150});

  // Filling the entire rect should work.
  virtualContext.fillRect(0, 0, 200, 150);

  // Too much width should not work.
  expect(() => {
    virtualContext.fillRect(0, 0, 201, 150);
  }).toThrow();

  expect(() => {
    virtualContext.fillRect(1, 0, 200, 150);
  }).toThrow();

  // Being too far to the left should not work.
  expect(() => {
    virtualContext.fillRect(-1, 0, 200, 150);
  }).toThrow();

  // Too much height should not work.
  expect(() => {
    virtualContext.fillRect(0, 0, 200, 151);
  }).toThrow();

  expect(() => {
    virtualContext.fillRect(0, 1, 200, 150);
  }).toThrow();

  // Being too far to the top should not work.
  expect(() => {
    virtualContext.fillRect(0, -1, 200, 150);
  }).toThrow();
});


test('nested virtual canvas contexts work', async () => {
  const ctx = setupCanvasContext();
  const mockCalls = (ctx.moveTo as Mock).mock.calls;
  const virtualContext =
      new ChildVirtualContext(ctx, {x: 100, y: 200, width: 200, height: 150});
  const virtualContext2 = new ChildVirtualContext(
      virtualContext, {x: 10, y: 10, width: 10, height: 10});

  virtualContext2.moveTo(10, 5);
  expect(mockCalls[0]).toEqual([120, 215]);
});
