// Linux JNI for Java DataSource benchmark.
// Links against libperfetto_c, no Android deps.

#include <jni.h>
#include <string.h>

#include "perfetto/public/abi/atomic.h"
#include "perfetto/public/abi/backend_type.h"
#include "perfetto/public/abi/data_source_abi.h"
#include "perfetto/public/abi/producer_abi.h"
#include "perfetto/public/abi/stream_writer_abi.h"
#include "perfetto/public/abi/tracing_session_abi.h"
#include "perfetto/public/producer.h"
#include "perfetto/public/stream_writer.h"
#include "perfetto/public/tracing_session.h"

static struct PerfettoDsImpl* g_ds_impl = nullptr;
static PERFETTO_ATOMIC(bool) * g_enabled_ptr = nullptr;

extern "C" {

JNIEXPORT void JNICALL Java_dev_perfetto_sdk_PerfettoBench_nativeInit(JNIEnv*,
                                                                      jclass) {
  struct PerfettoProducerInitArgs args = PERFETTO_PRODUCER_INIT_ARGS_INIT();
  args.backends = PERFETTO_BACKEND_IN_PROCESS;
  PerfettoProducerInit(args);
}

JNIEXPORT jlong JNICALL
Java_dev_perfetto_sdk_PerfettoBench_nativeRegisterDs(JNIEnv* env,
                                                     jclass,
                                                     jstring name) {
  const char* name_c = env->GetStringUTFChars(name, nullptr);
  size_t name_len = strlen(name_c);

  struct PerfettoDsImpl* ds_impl = PerfettoDsImplCreate();

  // Build DataSourceDescriptor: field 1 = name
  uint8_t desc[256];
  uint8_t* p = desc;
  *p++ = (1 << 3) | 2;
  *p++ = static_cast<uint8_t>(name_len);
  memcpy(p, name_c, name_len);
  p += name_len;

  PERFETTO_ATOMIC(bool)* enabled_ptr = nullptr;
  bool ok = PerfettoDsImplRegister(ds_impl, &enabled_ptr, desc,
                                   static_cast<size_t>(p - desc));
  env->ReleaseStringUTFChars(name, name_c);

  if (!ok) {
    return 0;
  }

  g_ds_impl = ds_impl;
  g_enabled_ptr = enabled_ptr;
  return static_cast<jlong>(reinterpret_cast<uintptr_t>(ds_impl));
}

JNIEXPORT jlong JNICALL
Java_dev_perfetto_sdk_PerfettoBench_nativeStartSession(JNIEnv* env,
                                                       jclass,
                                                       jstring ds_name) {
  const char* name_c = env->GetStringUTFChars(ds_name, nullptr);
  size_t name_len = strlen(name_c);

  // Build TraceConfig proto manually:
  // TraceConfig {
  //   buffers { size_kb: 4096 }
  //   data_sources { config { name: "<ds_name>" } }
  // }
  uint8_t cfg[256];
  size_t pos = 0;

  // buffers (field 1, nested)
  cfg[pos++] = (1 << 3) | 2;  // tag
  cfg[pos++] = 3;             // len
  // BufferConfig.size_kb (field 1, varint) = 4096
  cfg[pos++] = (1 << 3) | 0;
  cfg[pos++] = 0x80;
  cfg[pos++] = 0x20;  // 4096

  // data_sources (field 2, nested)
  cfg[pos++] = (2 << 3) | 2;  // tag
  size_t ds_len_pos = pos++;  // len placeholder

  // DataSource.config (field 1, nested)
  cfg[pos++] = (1 << 3) | 2;      // tag
  size_t config_len_pos = pos++;  // len placeholder

  // DataSourceConfig.name (field 1, string)
  cfg[pos++] = (1 << 3) | 2;
  cfg[pos++] = static_cast<uint8_t>(name_len);
  memcpy(cfg + pos, name_c, name_len);
  pos += name_len;

  // Backfill lengths
  cfg[config_len_pos] = static_cast<uint8_t>(pos - config_len_pos - 1);
  cfg[ds_len_pos] = static_cast<uint8_t>(pos - ds_len_pos - 1);

  env->ReleaseStringUTFChars(ds_name, name_c);

  struct PerfettoTracingSessionImpl* session =
      PerfettoTracingSessionCreate(PERFETTO_BACKEND_IN_PROCESS);
  PerfettoTracingSessionSetup(session, cfg, pos);
  PerfettoTracingSessionStartBlocking(session);

  return static_cast<jlong>(reinterpret_cast<uintptr_t>(session));
}

JNIEXPORT void JNICALL
Java_dev_perfetto_sdk_PerfettoBench_nativeStopSession(JNIEnv*,
                                                      jclass,
                                                      jlong session_ptr) {
  auto* session = reinterpret_cast<struct PerfettoTracingSessionImpl*>(
      static_cast<uintptr_t>(session_ptr));
  PerfettoTracingSessionStopBlocking(session);
  PerfettoTracingSessionDestroy(session);
}

JNIEXPORT void JNICALL
Java_dev_perfetto_sdk_PerfettoBench_nativeWritePacket(JNIEnv* env,
                                                      jclass,
                                                      jlong ds_ptr,
                                                      jbyteArray buf,
                                                      jint len) {
  auto* ds_impl =
      reinterpret_cast<struct PerfettoDsImpl*>(static_cast<uintptr_t>(ds_ptr));

  // Same path as our real JNI: copy to stack, iterate, AppendBytes.
  uint8_t stack_buf[4096];
  env->GetByteArrayRegion(buf, 0, len, reinterpret_cast<jbyte*>(stack_buf));

  struct PerfettoDsImplTracerIterator it =
      PerfettoDsImplTraceIterateBegin(ds_impl);
  while (it.tracer) {
    struct PerfettoStreamWriter writer =
        PerfettoDsTracerImplPacketBegin(it.tracer);
    PerfettoStreamWriterAppendBytes(&writer, stack_buf,
                                    static_cast<size_t>(len));
    PerfettoDsTracerImplPacketEnd(it.tracer, &writer);
    PerfettoDsImplTraceIterateNext(ds_impl, &it);
  }
}

}  // extern "C"
