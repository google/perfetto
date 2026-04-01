/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_JAVA_DATASOURCE_JNI_PERFETTO_DATASOURCE_JNI_H_
#define SRC_JAVA_DATASOURCE_JNI_PERFETTO_DATASOURCE_JNI_H_

#include <jni.h>

namespace perfetto {
namespace jni {

int register_dev_perfetto_sdk_PerfettoDataSource(JNIEnv* env);

}  // namespace jni
}  // namespace perfetto

#endif  // SRC_JAVA_DATASOURCE_JNI_PERFETTO_DATASOURCE_JNI_H_
