/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/java_sdk/main/cpp/com_google_perfetto_sdk_PerfettoExampleWrapper.h"

#include <jni.h>

#include <string>

#include "src/java_sdk/main/cpp/example.h"

JNIEXPORT jint JNICALL
Java_com_google_perfetto_sdk_PerfettoExampleWrapper_runPerfettoMain(
    JNIEnv* env,
    jobject thiz,
    jstring outputFilePath) {
  const char* cstr = env->GetStringUTFChars(outputFilePath, NULL);
  std::string file_path = std::string(cstr);
  env->ReleaseStringUTFChars(outputFilePath, cstr);
  return run_main(file_path);
}