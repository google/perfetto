// Copyright 2014 The Chromium Authors. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//    * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//    * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//    * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

import {getFontContent, IChartConfig} from '../config';
import {drawCanvasLabel, measureTextWidth} from './utils';

export class Label {
  readonly content: string;

  width: number | undefined;

  private config: IChartConfig;

  constructor(content: string, config: IChartConfig) {
    this.content = content;
    this.config = config;
  }

  private trimMiddle(str: string, maxLength: number): string {
    if (str.length <= maxLength) {
      return String(str);
    }

    let leftHalf = maxLength >> 1;
    let rightHalf = maxLength - leftHalf - 1;
    if ((str.codePointAt(str.length - rightHalf - 1) as number) >= 0x10000) {
      --rightHalf;
      ++leftHalf;
    }
    if (leftHalf > 0 && (str.codePointAt(leftHalf - 1) as number) >= 0x10000) {
      --leftHalf;
    }
    return `${str.substr(0, leftHalf)}…${str.substr(
      str.length - rightHalf,
      rightHalf,
    )}`;
  }

  // ref -> https://github.com/ChromeDevTools/devtools-frontend/blob/fd72c071cc7869c6d6fcda7d500558d8ad5ff7be/front_end/ui/legacy/UIUtils.ts#L1422
  private trimText(
    context: CanvasRenderingContext2D,
    text: string,
    maxWidth: number,
    trimFunction: (arg0: string, arg1: number) => string,
  ): string {
    const maxLength = 200;
    if (maxWidth <= 10) {
      return '';
    }
    if (text.length > maxLength) {
      text = trimFunction(text, maxLength);
    }
    if (this.width === undefined || this.width <= maxWidth) {
      return text;
    }

    let l = 0;
    let r: number = text.length;
    let lv = 0;
    let rv: number = this.width;
    while (l < r && lv !== rv && lv !== maxWidth) {
      const m = Math.ceil(l + ((r - l) * (maxWidth - lv)) / (rv - lv));
      const mv = measureTextWidth(context, trimFunction(text, m));
      if (mv <= maxWidth) {
        l = m;
        lv = mv;
      } else {
        r = m - 1;
        rv = mv;
      }
    }
    text = trimFunction(text, l);
    return text !== '…' ? text : '';
  }

  private trimTextMiddle(
    ctx: CanvasRenderingContext2D,
    text: string,
    maxWidth: number,
  ): string {
    return this.trimText(ctx, text, maxWidth, (_text, _width) =>
      this.trimMiddle(_text, _width),
    );
  }

  draw(
    ctx: CanvasRenderingContext2D,
    pos: {
      x: number;
      y: number;
    },
    containerWidth: number,
    color = '#fdfdfd',
  ) {
    if (!this.content) {
      return;
    }
    if (this.width === undefined) {
      this.width = measureTextWidth(ctx, this.content);
    }
    const showContent = this.trimTextMiddle(
      ctx,
      this.content,
      Math.floor(containerWidth - 2 * (this.config.label?.padding ?? 0)),
    );
    drawCanvasLabel(
      ctx,
      {
        x: pos.x + (this.config.label?.padding ?? 0),
        y: pos.y + Math.floor((this.config.node?.height ?? 0) / 2),
      },
      showContent,
      `${getFontContent(this.config)}`,
      color,
    );
  }
}
