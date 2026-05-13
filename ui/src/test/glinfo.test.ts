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

import {test} from '@playwright/test';

/**
 * This test simply dumps WebGL information to the console. It's not a real
 * test, but it can be useful to quickly check the WebGL capabilities of the
 * environment where tests are running. It's very quick to run as it doesn't
 * load a trace or even load the Perfetto UI at all.
 */
test('Dump GL info', async ({browser}) => {
  const page = await browser.newPage();
  const info = await page.evaluate(() => {
    // Use separate canvases to accurately detect support for each WebGL version
    const canvas1 = document.createElement('canvas');
    const canvas2 = document.createElement('canvas');
    const webgl2 = canvas2.getContext('webgl2');
    const webgl1 = canvas1.getContext('webgl');
    const gl = webgl2 ?? webgl1;

    if (!gl) {
      return {
        error: 'WebGL not supported',
        webgl2Supported: false,
        webgl1Supported: false,
      };
    }

    const dbg = gl.getExtension('WEBGL_debug_renderer_info');
    const contextType = webgl2 ? 'webgl2' : 'webgl1';

    // Gather all available debug parameters
    const params: Record<string, unknown> = {
      contextType,
      webgl2Supported: !!webgl2,
      webgl1Supported: !!webgl1,
      debugExtensionAvailable: !!dbg,

      // Renderer info (from debug extension if available)
      renderer: dbg ? gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL) : 'N/A',
      vendor: dbg ? gl.getParameter(dbg.UNMASKED_VENDOR_WEBGL) : 'N/A',

      // Standard WebGL parameters
      version: gl.getParameter(gl.VERSION),
      shadingLanguageVersion: gl.getParameter(gl.SHADING_LANGUAGE_VERSION),
      vendorString: gl.getParameter(gl.VENDOR),
      rendererString: gl.getParameter(gl.RENDERER),

      // Capabilities
      maxTextureSize: gl.getParameter(gl.MAX_TEXTURE_SIZE),
      maxViewportDims: gl.getParameter(gl.MAX_VIEWPORT_DIMS),
      maxRenderbufferSize: gl.getParameter(gl.MAX_RENDERBUFFER_SIZE),
      maxVertexAttribs: gl.getParameter(gl.MAX_VERTEX_ATTRIBS),
      maxVertexUniformVectors: gl.getParameter(gl.MAX_VERTEX_UNIFORM_VECTORS),
      maxFragmentUniformVectors: gl.getParameter(
        gl.MAX_FRAGMENT_UNIFORM_VECTORS,
      ),
      maxVaryingVectors: gl.getParameter(gl.MAX_VARYING_VECTORS),
      maxTextureImageUnits: gl.getParameter(gl.MAX_TEXTURE_IMAGE_UNITS),
      maxCombinedTextureImageUnits: gl.getParameter(
        gl.MAX_COMBINED_TEXTURE_IMAGE_UNITS,
      ),

      // Precision formats (for fragment shaders)
      highFloatPrecision: gl.getShaderPrecisionFormat(
        gl.FRAGMENT_SHADER,
        gl.HIGH_FLOAT,
      ),
      mediumFloatPrecision: gl.getShaderPrecisionFormat(
        gl.FRAGMENT_SHADER,
        gl.MEDIUM_FLOAT,
      ),

      // Available extensions
      supportedExtensions: gl.getSupportedExtensions(),
    };

    return params;
  });
  console.log('GL INFO:', info);
});
