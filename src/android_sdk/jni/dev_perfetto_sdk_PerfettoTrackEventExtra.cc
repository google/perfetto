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

#include "src/android_sdk/jni/dev_perfetto_sdk_PerfettoTrackEventExtra.h"

#include <jni.h>
#include "src/android_sdk/jni/macros.h"
#include "src/android_sdk/jni/string_buffer.h"
#include "src/android_sdk/nativehelper/JNIHelp.h"
#include "src/android_sdk/nativehelper/scoped_utf_chars.h"
#include "src/android_sdk/perfetto_sdk_for_jni/tracing_sdk.h"

namespace perfetto {
namespace jni {

template <typename T>
inline static T* toPointer(jlong ptr) {
  return reinterpret_cast<T*>(static_cast<uintptr_t>(ptr));
}

template <typename T>
inline static jlong toJLong(T* ptr) {
  return static_cast<jlong>(reinterpret_cast<uintptr_t>(ptr));
}

static jlong dev_perfetto_sdk_PerfettoTrackEventExtraArg_init(JNIEnv* env,
                                                              jclass,
                                                              jstring name) {
  return toJLong(new sdk_for_jni::DebugArg(
      StringBuffer::utf16_to_ascii(env, name).data()));
}

static jlong dev_perfetto_sdk_PerfettoTrackEventExtraArg_delete(
    PERFETTO_JNI_HOST_PARAMS) {
  return toJLong(&sdk_for_jni::DebugArg::delete_arg);
}

static jlong dev_perfetto_sdk_PerfettoTrackEventExtraArg_get_extra_ptr(
    PERFETTO_JNI_HOST_PARAMS_COMMA jlong ptr) {
  sdk_for_jni::DebugArg* arg = toPointer<sdk_for_jni::DebugArg>(ptr);
  return toJLong(arg->get());
}

static void dev_perfetto_sdk_PerfettoTrackEventExtraArg_set_value_int64(
    PERFETTO_JNI_HOST_PARAMS_COMMA jlong ptr,
    jlong val) {
  sdk_for_jni::DebugArg* arg = toPointer<sdk_for_jni::DebugArg>(ptr);
  auto& arg_int64 = arg->get()->arg_int64;
  arg_int64.header.type = PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_INT64;
  arg_int64.name = arg->name();
  arg_int64.value = val;
}

static void dev_perfetto_sdk_PerfettoTrackEventExtraArg_set_value_bool(
    PERFETTO_JNI_HOST_PARAMS_COMMA jlong ptr,
    jboolean val) {
  sdk_for_jni::DebugArg* arg = toPointer<sdk_for_jni::DebugArg>(ptr);
  auto& arg_bool = arg->get()->arg_bool;
  arg_bool.header.type = PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_BOOL;
  arg_bool.name = arg->name();
  arg_bool.value = val;
}

static void dev_perfetto_sdk_PerfettoTrackEventExtraArg_set_value_double(
    PERFETTO_JNI_HOST_PARAMS_COMMA jlong ptr,
    jdouble val) {
  sdk_for_jni::DebugArg* arg = toPointer<sdk_for_jni::DebugArg>(ptr);
  auto& arg_double = arg->get()->arg_double;
  arg_double.header.type = PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_DOUBLE;
  arg_double.name = arg->name();
  arg_double.value = val;
}

static void dev_perfetto_sdk_PerfettoTrackEventExtraArg_set_value_string(
    JNIEnv* env,
    jclass,
    jlong ptr,
    jstring val) {
  sdk_for_jni::DebugArg* arg = toPointer<sdk_for_jni::DebugArg>(ptr);
  auto& arg_string = arg->get()->arg_string;
  arg_string.header.type = PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_STRING;
  arg_string.name = arg->name();
  arg_string.value = StringBuffer::utf16_to_ascii(env, val).data();
}

static jlong dev_perfetto_sdk_PerfettoTrackEventExtraField_init(
    PERFETTO_JNI_HOST_PARAMS) {
  return toJLong(new sdk_for_jni::ProtoField());
}

static jlong dev_perfetto_sdk_PerfettoTrackEventExtraFieldNested_init(
    PERFETTO_JNI_HOST_PARAMS) {
  return toJLong(new sdk_for_jni::ProtoFieldNested());
}

static jlong dev_perfetto_sdk_PerfettoTrackEventExtraField_delete(
    PERFETTO_JNI_HOST_PARAMS) {
  return toJLong(&sdk_for_jni::ProtoField::delete_field);
}

static jlong dev_perfetto_sdk_PerfettoTrackEventExtraFieldNested_delete(
    PERFETTO_JNI_HOST_PARAMS) {
  return toJLong(&sdk_for_jni::ProtoFieldNested::delete_field);
}

static jlong dev_perfetto_sdk_PerfettoTrackEventExtraField_get_extra_ptr(
    PERFETTO_JNI_HOST_PARAMS_COMMA jlong ptr) {
  sdk_for_jni::ProtoField* field = toPointer<sdk_for_jni::ProtoField>(ptr);
  return toJLong(field->get());
}

static jlong dev_perfetto_sdk_PerfettoTrackEventExtraFieldNested_get_extra_ptr(
    PERFETTO_JNI_HOST_PARAMS_COMMA jlong ptr) {
  sdk_for_jni::ProtoFieldNested* field =
      toPointer<sdk_for_jni::ProtoFieldNested>(ptr);
  return toJLong(field->get());
}

static void dev_perfetto_sdk_PerfettoTrackEventExtraField_set_value_int64(
    PERFETTO_JNI_HOST_PARAMS_COMMA jlong ptr,
    jlong id,
    jlong val) {
  sdk_for_jni::ProtoField* field = toPointer<sdk_for_jni::ProtoField>(ptr);
  auto& field_varint = field->get()->field_varint;
  field_varint.header.type = PERFETTO_TE_HL_PROTO_TYPE_VARINT;
  field_varint.header.id = static_cast<uint32_t>(id);
  field_varint.value = val;
}

static void dev_perfetto_sdk_PerfettoTrackEventExtraField_set_value_double(
    PERFETTO_JNI_HOST_PARAMS_COMMA jlong ptr,
    jlong id,
    jdouble val) {
  sdk_for_jni::ProtoField* field = toPointer<sdk_for_jni::ProtoField>(ptr);
  auto& field_double = field->get()->field_double;
  field_double.header.type = PERFETTO_TE_HL_PROTO_TYPE_DOUBLE;
  field_double.header.id = static_cast<uint32_t>(id);
  field_double.value = val;
}

static void dev_perfetto_sdk_PerfettoTrackEventExtraField_set_value_string(
    JNIEnv* env,
    jclass,
    jlong ptr,
    jlong id,
    jstring val) {
  sdk_for_jni::ProtoField* field = toPointer<sdk_for_jni::ProtoField>(ptr);
  auto& field_cstr = field->get()->field_cstr;
  field_cstr.header.type = PERFETTO_TE_HL_PROTO_TYPE_CSTR;
  field_cstr.header.id = static_cast<uint32_t>(id);
  field_cstr.str = StringBuffer::utf16_to_ascii(env, val).data();
}

static void
dev_perfetto_sdk_PerfettoTrackEventExtraField_set_value_with_interning(
    JNIEnv* env,
    jclass,
    jlong ptr,
    jlong id,
    jstring val,
    jlong interned_type_id) {
  sdk_for_jni::ProtoField* field = toPointer<sdk_for_jni::ProtoField>(ptr);
  auto& field_cstr = field->get()->field_cstr_interned;
  field_cstr.header.type = PERFETTO_TE_HL_PROTO_TYPE_CSTR_INTERNED;
  field_cstr.header.id = static_cast<uint32_t>(id);
  field_cstr.str = StringBuffer::utf16_to_ascii(env, val).data();
  field_cstr.interned_type_id = static_cast<uint32_t>(interned_type_id);
}

static void dev_perfetto_sdk_PerfettoTrackEventExtraFieldNested_add_field(
    PERFETTO_JNI_HOST_PARAMS_COMMA jlong field_ptr,
    jlong arg_ptr) {
  sdk_for_jni::ProtoFieldNested* field =
      toPointer<sdk_for_jni::ProtoFieldNested>(field_ptr);
  field->add_field(toPointer<PerfettoTeHlProtoField>(arg_ptr));
}

static void dev_perfetto_sdk_PerfettoTrackEventExtraFieldNested_set_id(
    PERFETTO_JNI_HOST_PARAMS_COMMA jlong ptr,
    jlong id) {
  sdk_for_jni::ProtoFieldNested* field =
      toPointer<sdk_for_jni::ProtoFieldNested>(ptr);
  field->set_id(id);
}

static jlong dev_perfetto_sdk_PerfettoTrackEventExtraFlow_init(
    PERFETTO_JNI_HOST_PARAMS) {
  return toJLong(new sdk_for_jni::Flow());
}

static void dev_perfetto_sdk_PerfettoTrackEventExtraFlow_set_process_flow(
    PERFETTO_JNI_HOST_PARAMS_COMMA jlong ptr,
    jlong id) {
  sdk_for_jni::Flow* flow = toPointer<sdk_for_jni::Flow>(ptr);
  flow->set_process_flow(id);
}

static void
dev_perfetto_sdk_PerfettoTrackEventExtraFlow_set_process_terminating_flow(
    PERFETTO_JNI_HOST_PARAMS_COMMA jlong ptr,
    jlong id) {
  sdk_for_jni::Flow* flow = toPointer<sdk_for_jni::Flow>(ptr);
  flow->set_process_terminating_flow(id);
}

static jlong dev_perfetto_sdk_PerfettoTrackEventExtraFlow_delete(
    PERFETTO_JNI_HOST_PARAMS) {
  return toJLong(&sdk_for_jni::Flow::delete_flow);
}

static jlong dev_perfetto_sdk_PerfettoTrackEventExtraFlow_get_extra_ptr(
    PERFETTO_JNI_HOST_PARAMS_COMMA jlong ptr) {
  sdk_for_jni::Flow* flow = toPointer<sdk_for_jni::Flow>(ptr);
  return toJLong(flow->get());
}

static jlong dev_perfetto_sdk_PerfettoTrackEventExtraNamedTrack_init(
    JNIEnv* env,
    jclass,
    jlong id,
    jstring name,
    jlong parent_uuid,
    jboolean is_name_static) {
  return toJLong(new sdk_for_jni::NamedTrack(
      id, parent_uuid, StringBuffer::utf16_to_ascii(env, name).data(),
      is_name_static));
}

static jlong dev_perfetto_sdk_PerfettoTrackEventExtraNamedTrack_delete(
    PERFETTO_JNI_HOST_PARAMS) {
  return toJLong(&sdk_for_jni::NamedTrack::delete_track);
}

static jlong dev_perfetto_sdk_PerfettoTrackEventExtraNamedTrack_get_extra_ptr(
    PERFETTO_JNI_HOST_PARAMS_COMMA jlong ptr) {
  sdk_for_jni::NamedTrack* track = toPointer<sdk_for_jni::NamedTrack>(ptr);
  return toJLong(track->get());
}

static jlong dev_perfetto_sdk_PerfettoTrackEventExtraCounterTrack_init(
    JNIEnv* env,
    jclass,
    jstring name,
    jlong parent_uuid,
    jboolean is_name_static) {
  return toJLong(new sdk_for_jni::RegisteredTrack(
      1, parent_uuid, StringBuffer::utf16_to_ascii(env, name).data(), true,
      is_name_static));
}

static jlong dev_perfetto_sdk_PerfettoTrackEventExtraCounterTrack_delete(
    PERFETTO_JNI_HOST_PARAMS) {
  return toJLong(&sdk_for_jni::RegisteredTrack::delete_track);
}

static jlong dev_perfetto_sdk_PerfettoTrackEventExtraCounterTrack_get_extra_ptr(
    PERFETTO_JNI_HOST_PARAMS_COMMA jlong ptr) {
  sdk_for_jni::RegisteredTrack* track =
      toPointer<sdk_for_jni::RegisteredTrack>(ptr);
  return toJLong(track->get());
}

static jlong dev_perfetto_sdk_PerfettoTrackEventExtraCounter_init(
    PERFETTO_JNI_HOST_PARAMS) {
  return toJLong(new sdk_for_jni::Counter());
}

static jlong dev_perfetto_sdk_PerfettoTrackEventExtraCounter_delete(
    PERFETTO_JNI_HOST_PARAMS) {
  return toJLong(&sdk_for_jni::Counter::delete_counter);
}

static jlong dev_perfetto_sdk_PerfettoTrackEventExtraCounter_get_extra_ptr(
    PERFETTO_JNI_HOST_PARAMS_COMMA jlong ptr) {
  sdk_for_jni::Counter* counter = toPointer<sdk_for_jni::Counter>(ptr);
  return toJLong(counter->get());
}

static void dev_perfetto_sdk_PerfettoTrackEventExtraCounter_set_value_int64(
    PERFETTO_JNI_HOST_PARAMS_COMMA jlong ptr,
    jlong val) {
  sdk_for_jni::Counter* counter = toPointer<sdk_for_jni::Counter>(ptr);
  auto& counter_int64 = counter->get()->counter_int64;
  counter_int64.header.type = PERFETTO_TE_HL_EXTRA_TYPE_COUNTER_INT64;
  counter_int64.value = val;
}

static void dev_perfetto_sdk_PerfettoTrackEventExtraCounter_set_value_double(
    PERFETTO_JNI_HOST_PARAMS_COMMA jlong ptr,
    jdouble val) {
  sdk_for_jni::Counter* counter = toPointer<sdk_for_jni::Counter>(ptr);
  auto& counter_double = counter->get()->counter_double;
  counter_double.header.type = PERFETTO_TE_HL_EXTRA_TYPE_COUNTER_DOUBLE;
  counter_double.value = val;
}

static jlong dev_perfetto_sdk_PerfettoTrackEventExtra_init(
    PERFETTO_JNI_HOST_PARAMS) {
  return toJLong(new sdk_for_jni::Extra());
}

static jlong dev_perfetto_sdk_PerfettoTrackEventExtra_delete(
    PERFETTO_JNI_HOST_PARAMS) {
  return toJLong(&sdk_for_jni::Extra::delete_extra);
}

static void dev_perfetto_sdk_PerfettoTrackEventExtra_add_arg(
    PERFETTO_JNI_HOST_PARAMS_COMMA jlong extra_ptr,
    jlong arg_ptr) {
  sdk_for_jni::Extra* extra = toPointer<sdk_for_jni::Extra>(extra_ptr);
  extra->push_extra(toPointer<PerfettoTeHlExtra>(arg_ptr));
}

static void dev_perfetto_sdk_PerfettoTrackEventExtra_clear_args(
    PERFETTO_JNI_HOST_PARAMS_COMMA jlong ptr) {
  sdk_for_jni::Extra* extra = toPointer<sdk_for_jni::Extra>(ptr);
  extra->clear_extras();
}

static void dev_perfetto_sdk_PerfettoTrackEventExtra_emit(JNIEnv* env,
                                                          jclass,
                                                          jint type,
                                                          jlong cat_ptr,
                                                          jstring name,
                                                          jlong extra_ptr) {
  sdk_for_jni::Category* category = toPointer<sdk_for_jni::Category>(cat_ptr);
  trace_event(type, category->get(),
              StringBuffer::utf16_to_ascii(env, name).data(),
              toPointer<sdk_for_jni::Extra>(extra_ptr));
  StringBuffer::reset();
}

static jlong dev_perfetto_sdk_PerfettoTrackEventExtraProto_init(
    PERFETTO_JNI_HOST_PARAMS) {
  return toJLong(new sdk_for_jni::Proto());
}

static jlong dev_perfetto_sdk_PerfettoTrackEventExtraProto_delete(
    PERFETTO_JNI_HOST_PARAMS) {
  return toJLong(&sdk_for_jni::Proto::delete_proto);
}

static jlong dev_perfetto_sdk_PerfettoTrackEventExtraProto_get_extra_ptr(
    PERFETTO_JNI_HOST_PARAMS_COMMA jlong ptr) {
  sdk_for_jni::Proto* proto = toPointer<sdk_for_jni::Proto>(ptr);
  return toJLong(proto->get());
}

static void dev_perfetto_sdk_PerfettoTrackEventExtraProto_add_field(
    PERFETTO_JNI_HOST_PARAMS_COMMA long proto_ptr,
    jlong arg_ptr) {
  sdk_for_jni::Proto* proto = toPointer<sdk_for_jni::Proto>(proto_ptr);
  proto->add_field(toPointer<PerfettoTeHlProtoField>(arg_ptr));
}

static void dev_perfetto_sdk_PerfettoTrackEventExtraProto_clear_fields(
    PERFETTO_JNI_HOST_PARAMS_COMMA jlong ptr) {
  sdk_for_jni::Proto* proto = toPointer<sdk_for_jni::Proto>(ptr);
  proto->clear_fields();
}

static const JNINativeMethod gExtraMethods[] = {
    {"native_init", "()J",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtra_init},
    {"native_delete", "()J",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtra_delete},
    {"native_add_arg", "(JJ)V",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtra_add_arg},
    {"native_clear_args", "(J)V",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtra_clear_args},
    {"native_emit", "(IJLjava/lang/String;J)V",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtra_emit}};

static const JNINativeMethod gProtoMethods[] = {
    {"native_init", "()J",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraProto_init},
    {"native_delete", "()J",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraProto_delete},
    {"native_get_extra_ptr", "(J)J",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraProto_get_extra_ptr},
    {"native_add_field", "(JJ)V",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraProto_add_field},
    {"native_clear_fields", "(J)V",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraProto_clear_fields}};

static const JNINativeMethod gArgMethods[] = {
    {"native_init", "(Ljava/lang/String;)J",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraArg_init},
    {"native_delete", "()J",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraArg_delete},
    {"native_get_extra_ptr", "(J)J",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraArg_get_extra_ptr},
    {"native_set_value_int64", "(JJ)V",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraArg_set_value_int64},
    {"native_set_value_bool", "(JZ)V",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraArg_set_value_bool},
    {"native_set_value_double", "(JD)V",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraArg_set_value_double},
    {"native_set_value_string", "(JLjava/lang/String;)V",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraArg_set_value_string},
};

static const JNINativeMethod gFieldMethods[] = {
    {"native_init", "()J",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraField_init},
    {"native_delete", "()J",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraField_delete},
    {"native_get_extra_ptr", "(J)J",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraField_get_extra_ptr},
    {"native_set_value_int64", "(JJJ)V",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraField_set_value_int64},
    {"native_set_value_double", "(JJD)V",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraField_set_value_double},
    {"native_set_value_string", "(JJLjava/lang/String;)V",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraField_set_value_string},
    {"native_set_value_with_interning", "(JJLjava/lang/String;J)V",
     (void*)
         dev_perfetto_sdk_PerfettoTrackEventExtraField_set_value_with_interning},
};

static const JNINativeMethod gFieldNestedMethods[] = {
    {"native_init", "()J",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraFieldNested_init},
    {"native_delete", "()J",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraFieldNested_delete},
    {"native_get_extra_ptr", "(J)J",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraFieldNested_get_extra_ptr},
    {"native_add_field", "(JJ)V",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraFieldNested_add_field},
    {"native_set_id", "(JJ)V",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraFieldNested_set_id}};

static const JNINativeMethod gFlowMethods[] = {
    {"native_init", "()J",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraFlow_init},
    {"native_delete", "()J",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraFlow_delete},
    {"native_set_process_flow", "(JJ)V",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraFlow_set_process_flow},
    {"native_set_process_terminating_flow", "(JJ)V",
     (void*)
         dev_perfetto_sdk_PerfettoTrackEventExtraFlow_set_process_terminating_flow},
    {"native_get_extra_ptr", "(J)J",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraFlow_get_extra_ptr},
};

static const JNINativeMethod gNamedTrackMethods[] = {
    {"native_init", "(JLjava/lang/String;JZ)J",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraNamedTrack_init},
    {"native_delete", "()J",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraNamedTrack_delete},
    {"native_get_extra_ptr", "(J)J",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraNamedTrack_get_extra_ptr},
};

static const JNINativeMethod gCounterTrackMethods[] = {
    {"native_init", "(Ljava/lang/String;JZ)J",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraCounterTrack_init},
    {"native_delete", "()J",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraCounterTrack_delete},
    {"native_get_extra_ptr", "(J)J",
     (void*)
         dev_perfetto_sdk_PerfettoTrackEventExtraCounterTrack_get_extra_ptr}};

static const JNINativeMethod gCounterMethods[] = {
    {"native_init", "()J",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraCounter_init},
    {"native_delete", "()J",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraCounter_delete},
    {"native_get_extra_ptr", "(J)J",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraCounter_get_extra_ptr},
    {"native_set_value_int64", "(JJ)V",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraCounter_set_value_int64},
    {"native_set_value_double", "(JD)V",
     (void*)dev_perfetto_sdk_PerfettoTrackEventExtraCounter_set_value_double}};

int register_dev_perfetto_sdk_PerfettoTrackEventExtra(JNIEnv* env) {
  int res = jniRegisterNativeMethods(
      env,
      TO_MAYBE_JAR_JAR_CLASS_NAME(
          "dev/perfetto/sdk/PerfettoTrackEventExtra$Arg"),
      gArgMethods, NELEM(gArgMethods));
  LOG_ALWAYS_FATAL_IF(res < 0, "Unable to register arg native methods.");

  res = jniRegisterNativeMethods(
      env,
      TO_MAYBE_JAR_JAR_CLASS_NAME(
          "dev/perfetto/sdk/PerfettoTrackEventExtra$Field"),
      gFieldMethods, NELEM(gFieldMethods));
  LOG_ALWAYS_FATAL_IF(res < 0, "Unable to register field native methods.");

  res = jniRegisterNativeMethods(
      env,
      TO_MAYBE_JAR_JAR_CLASS_NAME(
          "dev/perfetto/sdk/PerfettoTrackEventExtra$FieldNested"),
      gFieldNestedMethods, NELEM(gFieldNestedMethods));
  LOG_ALWAYS_FATAL_IF(res < 0,
                      "Unable to register field nested native methods.");

  res = jniRegisterNativeMethods(
      env,
      TO_MAYBE_JAR_JAR_CLASS_NAME("dev/perfetto/sdk/PerfettoTrackEventExtra"),
      gExtraMethods, NELEM(gExtraMethods));
  LOG_ALWAYS_FATAL_IF(res < 0, "Unable to register extra native methods.");

  res = jniRegisterNativeMethods(
      env,
      TO_MAYBE_JAR_JAR_CLASS_NAME(
          "dev/perfetto/sdk/PerfettoTrackEventExtra$Proto"),
      gProtoMethods, NELEM(gProtoMethods));
  LOG_ALWAYS_FATAL_IF(res < 0, "Unable to register proto native methods.");

  res = jniRegisterNativeMethods(
      env,
      TO_MAYBE_JAR_JAR_CLASS_NAME(
          "dev/perfetto/sdk/PerfettoTrackEventExtra$Flow"),
      gFlowMethods, NELEM(gFlowMethods));
  LOG_ALWAYS_FATAL_IF(res < 0, "Unable to register flow native methods.");

  res = jniRegisterNativeMethods(
      env,
      TO_MAYBE_JAR_JAR_CLASS_NAME(
          "dev/perfetto/sdk/PerfettoTrackEventExtra$NamedTrack"),
      gNamedTrackMethods, NELEM(gNamedTrackMethods));
  LOG_ALWAYS_FATAL_IF(res < 0,
                      "Unable to register named track native methods.");

  res = jniRegisterNativeMethods(
      env,
      TO_MAYBE_JAR_JAR_CLASS_NAME(
          "dev/perfetto/sdk/PerfettoTrackEventExtra$CounterTrack"),
      gCounterTrackMethods, NELEM(gCounterTrackMethods));
  LOG_ALWAYS_FATAL_IF(res < 0,
                      "Unable to register counter track native methods.");

  res = jniRegisterNativeMethods(
      env,
      TO_MAYBE_JAR_JAR_CLASS_NAME(
          "dev/perfetto/sdk/PerfettoTrackEventExtra$Counter"),
      gCounterMethods, NELEM(gCounterMethods));
  LOG_ALWAYS_FATAL_IF(res < 0, "Unable to register counter native methods.");

  return 0;
}

}  // namespace jni
}  // namespace perfetto
