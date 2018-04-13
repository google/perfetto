/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include <jni.h>

#include "perfetto/traced/traced.h"

#include "src/base/test/test_task_runner.h"
#include "src/tracing/ipc/default_socket.h"

#include "test/fake_producer.h"

namespace perfetto {
namespace {
void ListenAndRespond(const std::string& name) {
  base::TestTaskRunner task_runner;
  FakeProducer producer(name);
  producer.Connect(GetProducerSocket(), &task_runner, [] {});
  task_runner.Run();
}
}  // namespace
}  // namespace perfetto

extern "C" JNIEXPORT void JNICALL
Java_android_perfetto_producer_ProducerActivity_setupProducer(JNIEnv*, jclass) {
  perfetto::ListenAndRespond("android.perfetto.cts.ProducerActivity");
}

extern "C" JNIEXPORT void JNICALL
Java_android_perfetto_producer_ProducerIsolatedService_setupProducer(JNIEnv*,
                                                                     jclass) {
  perfetto::ListenAndRespond("android.perfetto.cts.ProducerIsolatedService");
}

extern "C" JNIEXPORT void JNICALL
Java_android_perfetto_producer_ProducerService_setupProducer(JNIEnv*, jclass) {
  perfetto::ListenAndRespond("android.perfetto.cts.ProducerService");
}
