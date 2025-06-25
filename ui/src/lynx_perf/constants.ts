// Copyright (C) 2025 The Android Open Source Project
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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

export const LYNX_ISSUES_PLUGIN_ID = 'lynx.Perf';
export const LYNX_PERF_ELEMENT_PLUGIN_ID = 'lynx.Element';
export const LYNX_ISSUE_TYPE_DOM = 'dom';
export const LYNX_VITAL_TIMESTAMP_PLUGIN_ID = 'lynx.VitalTimestamp';
export const LYNX_NATIVE_MODULE_ID = 'lynx.NativeModule';

export const LYNX_BACKGROUND_THREAD_NAME = 'Lynx_JS';
export const LYNX_FRAME_JANK_PLUGIN_ID = 'lynx.FrameJank';

export const SLICE_LAYOUT_FIT_CONTENT_DEFAULTS = Object.freeze({
  padding: 3,
  rowSpacing: 0,
  heightMode: 'FIT_CONTENT',
  sliceHeight: 18,
});
// Timing
export const TIMING_PAINT_END = [
  'Timing::Mark.draw_end',
  'Timing::Mark.paintEnd',
];
export const PAINT_END_KEYS = ['draw_end', 'paintEnd'];
export const CRUCIAL_TIMING_KEYS = [
  'update_set_state_trigger',
  'OnPipelineStart',
  ...PAINT_END_KEYS,
];
export const TIMING_LOAD_BUNDLE_START = [
  'Timing::Mark.setup_load_template_start',
  'Timing::Mark.loadBundleStart',
];
export const TIMING_MARK_PREFIX = 'Timing::Mark.';
export const TIMING_MARK_FRAMEWORK_PREFIX = 'Timing::MarkFrameWorkTiming.';
export const INSTANCE_ID = 'instance_id';
export const TIMING_FLAGS = 'timing_flags';
export const PIPELINE_ID = 'pipeline_id';
// NativeModule Timing
export const NATIVEMODULE_CALL = 'CallJSB';
export const NATIVEMODULE_FUNC_CALL_START = 'JSBTiming::jsb_func_call_start';
export const NATIVEMODULE_CONVERT_PARAMS_END =
  'JSBTiming::jsb_func_convert_params_end';
export const NATIVEMODULE_THREAD_SWITCH_START =
  'JSBTiming::jsb_callback_thread_switch_start';
export const NATIVEMODULE_THREAD_SWITCH_END =
  'JSBTiming::jsb_callback_thread_switch_end';
export const NATIVEMODULE_CALLBACK_CONVERT_PARAMS_END =
  'JSBTiming::jsb_callback_convert_params_end';
export const NATIVEMODULE_CALLBACK_INVOKE_END =
  'JSBTiming::jsb_callback_invoke_end';
export const NATIVEMODULE_PLATFORM_METHOD_END =
  'JSBTiming::jsb_func_platform_method_end';
export const NATIVEMODULE_TIMING_FLUSH = 'JSBTiming::Flush';
export const NATIVEMODULE_NETWORK_REQUEST = 'Network::SendNetworkRequest';
// Frame Jank
export const DROP_FRAME_THRESHOLD = 16666666;
