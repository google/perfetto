/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "src/traced/probes/statsd_client/statsd_data_source.h"

#include <stdlib.h>

#include "perfetto/base/task_runner.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/subprocess.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/tracing/core/data_source_config.h"

#include "protos/perfetto/config/statsd/statsd_tracing_config.pbzero.h"
#include "protos/perfetto/trace/statsd/statsd_atom.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/third_party/statsd/shell_config.pbzero.h"

using ::perfetto::protos::pbzero::StatsdPullAtomConfig;
using ::perfetto::protos::pbzero::StatsdShellSubscription;
using ::perfetto::protos::pbzero::StatsdTracingConfig;

namespace perfetto {
namespace {

static constexpr const size_t kHeaderSize = sizeof(size_t);

void AddPullAtoms(const StatsdPullAtomConfig::Decoder& cfg,
                  protozero::RepeatedFieldIterator<int32_t> it,
                  StatsdShellSubscription* msg) {
  constexpr int32_t kDefaultPullFreqMs = 5000;
  int32_t pull_freq_ms = kDefaultPullFreqMs;
  if (cfg.has_pull_frequency_ms()) {
    pull_freq_ms = cfg.pull_frequency_ms();
  }

  for (; it; ++it) {
    auto* pulled_msg = msg->add_pulled();
    pulled_msg->set_freq_millis(pull_freq_ms);

    for (auto package = cfg.packages(); package; ++package) {
      pulled_msg->add_packages(*package);
    }

    auto* matcher_msg = pulled_msg->set_matcher();
    matcher_msg->set_atom_id(*it);
  }
}

void AddPushAtoms(protozero::RepeatedFieldIterator<int32_t> it,
                  StatsdShellSubscription* msg) {
  for (; it; ++it) {
    auto* matcher_msg = msg->add_pushed();
    matcher_msg->set_atom_id(*it);
  }
}

// Exec "cmd stats data-subscribe" and read/write stdin/stdout. This is
// the only way to make this work when side loading but for in tree
// builds this causes to many denials:
// avc: denied { execute_no_trans } for comm="traced_probes"
// path="/system/bin/cmd" dev="dm-0" ino=200 scontext=u:r:traced_probes:s0
// tcontext=u:object_r:system_file:s0 tclass=file permissive=1 avc: denied {
// call } for comm="cmd" scontext=u:r:traced_probes:s0 tcontext=u:r:statsd:s0
// tclass=binder permissive=1 avc: denied { use } for comm="cmd"
// path="pipe:[51149]" dev="pipefs" ino=51149 scontext=u:r:statsd:s0
// tcontext=u:r:traced_probes:s0 tclass=fd permissive=1 avc: denied { read } for
// comm="cmd" path="pipe:[51149]" dev="pipefs" ino=51149 scontext=u:r:statsd:s0
// tcontext=u:r:traced_probes:s0 tclass=fifo_file permissive=1 avc: denied {
// write } for comm="cmd" path="pipe:[51148]" dev="pipefs" ino=51148
// scontext=u:r:statsd:s0 tcontext=u:r:traced_probes:s0 tclass=fifo_file
// permissive=1 avc: denied { transfer } for comm="cmd"
// scontext=u:r:traced_probes:s0 tcontext=u:r:statsd:s0 tclass=binder
// permissive=1
class ExecStatsdBackend : public StatsdBackend {
 public:
  ExecStatsdBackend(std::string input, base::ScopedFile output_wr);
  virtual ~ExecStatsdBackend() override;

 private:
  base::Subprocess subprocess_;
};

ExecStatsdBackend::ExecStatsdBackend(std::string input,
                                     base::ScopedFile output_wr)
    : StatsdBackend(std::move(input), std::move(output_wr)),
      subprocess_({"/system/bin/cmd", "stats", "data-subscribe"}) {
  subprocess_.args.stdin_mode = base::Subprocess::InputMode::kBuffer;
  subprocess_.args.stdout_mode = base::Subprocess::OutputMode::kFd;
  subprocess_.args.stderr_mode = base::Subprocess::OutputMode::kInherit;
  subprocess_.args.input = std::move(input_);
  subprocess_.args.out_fd = std::move(output_wr_);
  subprocess_.Start();
  // Have to Poll at least once so the subprocess has a chance to
  // consume the input.
  // TODO(hjd): Might not manage to push the whole stdin here in which
  // case we can be stuck here forever. We should re-posttask the Poll
  // until the whole stdin is consumed.
  subprocess_.Poll();
}

// virtual
ExecStatsdBackend::~ExecStatsdBackend() = default;

std::unique_ptr<StatsdBackend> GetStatsdBackend(std::string in,
                                                base::ScopedFile&& out) {
  return std::unique_ptr<StatsdBackend>(
      new ExecStatsdBackend(std::move(in), std::move(out)));
}

}  // namespace

StatsdBackend::StatsdBackend(std::string input, base::ScopedFile output_wr)
    : input_(std::move(input)), output_wr_(std::move(output_wr)) {}

StatsdBackend::~StatsdBackend() = default;

SizetPrefixedMessageReader::SizetPrefixedMessageReader()
    : RingBufferMessageReader() {}
SizetPrefixedMessageReader::~SizetPrefixedMessageReader() {}

SizetPrefixedMessageReader::Message SizetPrefixedMessageReader::TryReadMessage(
    const uint8_t* start,
    const uint8_t* end) {
  SizetPrefixedMessageReader::Message msg{};
  size_t available = static_cast<size_t>(end - start);
  if (kHeaderSize <= available) {
    size_t sz = 0;
    static_assert(sizeof(sz) == kHeaderSize, "kHeaderSize must match size_t");
    memcpy(&sz, start, kHeaderSize);
    // It is valid for sz to be zero here and we must ensure we return
    // a valid Message for this case.
    if (kHeaderSize + sz <= available) {
      PERFETTO_CHECK(kHeaderSize + sz > sz);
      msg.start = start + kHeaderSize;
      msg.len = static_cast<uint32_t>(sz);
      msg.field_id = 0;
    }
  }
  return msg;
}

// static
const ProbesDataSource::Descriptor StatsdDataSource::descriptor = {
    /*name*/ "android.statsd",
    /*flags*/ Descriptor::kHandlesIncrementalState,
    /*fill_descriptor_func*/ nullptr,
};

StatsdDataSource::StatsdDataSource(base::TaskRunner* task_runner,
                                   TracingSessionID session_id,
                                   std::unique_ptr<TraceWriter> writer,
                                   const DataSourceConfig& ds_config)
    : ProbesDataSource(session_id, &descriptor),
      task_runner_(task_runner),
      writer_(std::move(writer)),
      output_(base::Pipe::Create(base::Pipe::Flags::kRdNonBlock)),
      shell_subscription_(GenerateShellConfig(ds_config)),
      weak_factory_(this) {}

StatsdDataSource::~StatsdDataSource() {
  if (output_.rd) {
    task_runner_->RemoveFileDescriptorWatch(output_.rd.get());
  }
}

void StatsdDataSource::Start() {
  // Don't bother actually connecting to statsd if no pull/push atoms
  // were configured:
  if (shell_subscription_.empty()) {
    PERFETTO_LOG("Empty statsd config. Not connecting to statsd.");
    return;
  }

  // The binary protocol for talking to statsd is to write 'size_t'
  // followed by a proto encoded ShellConfig. For now we assume that
  // us and statsd are the same bitness & endianness.
  std::string body = shell_subscription_;
  size_t size = body.size();
  static_assert(sizeof(size) == kHeaderSize, "kHeaderSize must match size_t");
  std::string input(sizeof(size) + size, '\0');
  memcpy(&input[0], &size, sizeof(size));
  memcpy(&input[0] + sizeof(size), body.data(), size);

  backend_ = GetStatsdBackend(std::move(input), std::move(output_.wr));

  // Watch is removed on destruction.
  auto weak_this = weak_factory_.GetWeakPtr();
  task_runner_->AddFileDescriptorWatch(output_.rd.get(), [weak_this] {
    if (weak_this) {
      weak_this->OnStatsdWakeup();
    }
  });
}

// Once the pipe is available to read we want to drain it but we need
// to split the work across multiple tasks to avoid statsd ddos'ing us
// and causing us to hit the timeout. At the same time we don't want
// multiple OnStatsdWakeup to cause 'concurrent' read cycles (we're
// single threaded so we can't actually race but we could still end up
// in some confused state) so:
// - The first wakeup triggers DoRead and sets read_in_progress_
// - Subsequent wakeups are ignored due to read_in_progress_
// - DoRead does a single read and either:
//    - No data = we're finished so unset read_in_progress_
//    - Some data so PostTask another DoRead.
void StatsdDataSource::OnStatsdWakeup() {
  if (read_in_progress_) {
    return;
  }
  read_in_progress_ = true;
  DoRead();
}

// Do a single read. If there is potentially more data to read schedule
// another DoRead.
void StatsdDataSource::DoRead() {
  PERFETTO_CHECK(read_in_progress_);

  uint8_t data[4098];
  // Read into the static buffer
  ssize_t rd = PERFETTO_EINTR(read(output_.rd.get(), &data, sizeof(data)));
  if (rd < 0) {
    if (!base::IsAgain(errno)) {
      PERFETTO_PLOG("Failed to read statsd pipe (ret: %zd)", rd);
    }
    // EAGAIN or otherwise we're done so re-enable the fd watch.
    read_in_progress_ = false;
    return;
  } else if (rd == 0) {
    // EOF so clean everything up.
    read_in_progress_ = false;
    task_runner_->RemoveFileDescriptorWatch(output_.rd.get());
    backend_.reset();
  }

  buffer_.Append(data, static_cast<size_t>(rd));

  TraceWriter::TracePacketHandle packet;
  for (;;) {
    SizetPrefixedMessageReader::Message msg = buffer_.ReadMessage();
    // The whole packet is not available so we're done.
    if (!msg.valid()) {
      break;
    }

    // A heart beat packet with no content
    if (msg.len == 0) {
      continue;
    }

    packet = writer_->NewTracePacket();
    // This is late. It's already been >=2 IPC hops since the client
    // code actually produced the atom however we don't get any time
    // stamp from statsd/the client so this is the best we can do:
    packet->set_timestamp(static_cast<uint64_t>(base::GetBootTimeNs().count()));
    auto* atom = packet->set_statsd_atom();
    atom->AppendRawProtoBytes(msg.start, msg.len);
    packet->Finalize();
  }

  // Potentially more to read so repost:
  auto weak_this = weak_factory_.GetWeakPtr();
  task_runner_->PostTask([weak_this] {
    if (weak_this) {
      weak_this->DoRead();
    }
  });
}

// static
std::string StatsdDataSource::GenerateShellConfig(
    const DataSourceConfig& config) {
  StatsdTracingConfig::Decoder cfg(config.statsd_tracing_config_raw());
  protozero::HeapBuffered<StatsdShellSubscription> msg;
  for (auto pull_it = cfg.pull_config(); pull_it; ++pull_it) {
    StatsdPullAtomConfig::Decoder pull_cfg(*pull_it);
    AddPullAtoms(pull_cfg, pull_cfg.raw_pull_atom_id(), msg.get());
    AddPullAtoms(pull_cfg, pull_cfg.pull_atom_id(), msg.get());
  }
  AddPushAtoms(cfg.push_atom_id(), msg.get());
  AddPushAtoms(cfg.raw_push_atom_id(), msg.get());
  return msg.SerializeAsString();
}

base::WeakPtr<StatsdDataSource> StatsdDataSource::GetWeakPtr() const {
  return weak_factory_.GetWeakPtr();
}

void StatsdDataSource::Flush(FlushRequestID, std::function<void()> callback) {
  writer_->Flush(callback);
}

void StatsdDataSource::ClearIncrementalState() {}

}  // namespace perfetto
