// Copyright (C) 2023 The Android Open Source Project
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

import {
  elementIsEditable,
  findRef,
  isOrContains,
  toHTMLElement,
} from './dom_utils';

describe('isOrContains', () => {
  const parent = document.createElement('div');
  const child = document.createElement('div');
  parent.appendChild(child);

  it('finds child in parent', () => {
    expect(isOrContains(parent, child)).toBeTruthy();
  });

  it('finds child in child', () => {
    expect(isOrContains(child, child)).toBeTruthy();
  });

  it('does not find parent in child', () => {
    expect(isOrContains(child, parent)).toBeFalsy();
  });
});

describe('findRef', () => {
  const parent = document.createElement('div');
  const fooChild = document.createElement('div');
  fooChild.setAttribute('ref', 'foo');
  parent.appendChild(fooChild);
  const barChild = document.createElement('div');
  barChild.setAttribute('ref', 'bar');
  parent.appendChild(barChild);

  it('should find refs in parent divs', () => {
    expect(findRef(parent, 'foo')).toEqual(fooChild);
    expect(findRef(parent, 'bar')).toEqual(barChild);
  });

  it('should find refs in self divs', () => {
    expect(findRef(fooChild, 'foo')).toEqual(fooChild);
    expect(findRef(barChild, 'bar')).toEqual(barChild);
  });

  it('should fail to find ref in unrelated divs', () => {
    const unrelated = document.createElement('div');
    expect(findRef(unrelated, 'foo')).toBeNull();
    expect(findRef(fooChild, 'bar')).toBeNull();
    expect(findRef(barChild, 'foo')).toBeNull();
  });
});

describe('toHTMLElement', () => {
  it('should convert a div to an HTMLElement', () => {
    const divElement: Element = document.createElement('div');
    expect(toHTMLElement(divElement)).toEqual(divElement);
  });

  it('should fail to convert an svg element to an HTMLElement', () => {
    const svgElement =
        document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    expect(() => toHTMLElement(svgElement)).toThrow(Error);
  });
});

describe('elementIsEditable', () => {
  test('text input', () => {
    const el = document.createElement('input');
    el.setAttribute('type', 'text');
    expect(elementIsEditable(el)).toBeTruthy();
  });

  test('radio input', () => {
    const el = document.createElement('input');
    el.setAttribute('type', 'radio');
    expect(elementIsEditable(el)).toBeFalsy();
  });

  test('checkbox input', () => {
    const el = document.createElement('input');
    el.setAttribute('type', 'checkbox');
    expect(elementIsEditable(el)).toBeFalsy();
  });

  test('button input', () => {
    const el = document.createElement('input');
    el.setAttribute('type', 'button');
    expect(elementIsEditable(el)).toBeFalsy();
  });

  test('div', () => {
    const el = document.createElement('div');
    expect(elementIsEditable(el)).toBeFalsy();
  });

  test('textarea', () => {
    const el = document.createElement('textarea');
    expect(elementIsEditable(el)).toBeTruthy();
  });

  test('nested', () => {
    const el = document.createElement('textarea');
    const nested = document.createElement('div');
    el.appendChild(nested);
    expect(elementIsEditable(nested)).toBeTruthy();
  });
});
