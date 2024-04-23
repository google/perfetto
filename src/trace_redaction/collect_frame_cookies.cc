/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_redaction/collect_frame_cookies.h"

#include "perfetto/base/status.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/proto_decoder.h"
#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/android/frame_timeline_event.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {

namespace {

using FrameTimelineEvent = protos::pbzero::FrameTimelineEvent;

struct Frame {
  uint32_t id;
  uint32_t pid;
  uint32_t cookie;
};

constexpr Frame kActualDisplayFrameStart = {
    FrameTimelineEvent::kActualDisplayFrameStartFieldNumber,
    FrameTimelineEvent::ActualDisplayFrameStart::kPidFieldNumber,
    FrameTimelineEvent::ActualDisplayFrameStart::kCookieFieldNumber,
};

constexpr Frame kExpectedDisplayFrameStart = {
    FrameTimelineEvent::kExpectedDisplayFrameStartFieldNumber,
    FrameTimelineEvent::ExpectedDisplayFrameStart::kPidFieldNumber,
    FrameTimelineEvent::ExpectedDisplayFrameStart::kCookieFieldNumber,
};

constexpr Frame kActualSurfaceFrameStart = {
    FrameTimelineEvent::kActualSurfaceFrameStartFieldNumber,
    FrameTimelineEvent::ActualSurfaceFrameStart::kPidFieldNumber,
    FrameTimelineEvent::ActualSurfaceFrameStart::kCookieFieldNumber,
};

constexpr Frame kExpectedSurfaceFrameStart = {
    FrameTimelineEvent::kExpectedSurfaceFrameStartFieldNumber,
    FrameTimelineEvent::ExpectedSurfaceFrameStart::kPidFieldNumber,
    FrameTimelineEvent::ExpectedSurfaceFrameStart::kCookieFieldNumber,
};

// Do not use `pid` from `kFrameEnd`.
constexpr Frame kFrameEnd = {
    FrameTimelineEvent::kFrameEndFieldNumber,
    0,
    FrameTimelineEvent::FrameEnd::kCookieFieldNumber,
};

}  // namespace

base::Status CollectFrameCookies::Begin(Context* context) const {
  if (context->global_frame_cookies.empty()) {
    return base::OkStatus();
  }

  return base::ErrStatus("FindFrameCookies: frame cookies already populated");
}

base::Status CollectFrameCookies::Collect(
    const protos::pbzero::TracePacket::Decoder& packet,
    Context* context) const {
  // A frame cookie needs a time and pid for a timeline query. Ignore packets
  // without a timestamp.
  if (!packet.has_timestamp() || !packet.has_frame_timeline_event()) {
    return base::OkStatus();
  }

  auto timestamp = packet.timestamp();

  // Only use the start frames. They are the only ones with a pid. End events
  // use the cookies to reference the pid in a start event.
  auto handlers = {
      kActualDisplayFrameStart,
      kActualSurfaceFrameStart,
      kExpectedDisplayFrameStart,
      kExpectedSurfaceFrameStart,
  };

  // Timeline Event Decoder.
  protozero::ProtoDecoder decoder(packet.frame_timeline_event());

  // If no handler worked, cookie will not get added to the global cookie field.
  for (const auto& handler : handlers) {
    auto outer = decoder.FindField(handler.id);

    if (!outer.valid()) {
      continue;
    }

    protozero::ProtoDecoder inner(outer.as_bytes());

    auto pid = inner.FindField(handler.pid);
    auto cookie = inner.FindField(handler.cookie);

    // This should be handled, but it is not valid. Drop the event by not adding
    // it to the global_frame_cookies list.
    if (!pid.valid() || !cookie.valid()) {
      continue;
    }

    FrameCookie frame_cookie;
    frame_cookie.pid = pid.as_int32();
    frame_cookie.cookie = cookie.as_int64();
    frame_cookie.ts = timestamp;

    context->global_frame_cookies.push_back(frame_cookie);

    break;
  }

  return base::OkStatus();
}

base::Status ReduceFrameCookies::Build(Context* context) const {
  if (!context->package_uid.has_value()) {
    return base::ErrStatus("ReduceFrameCookies: missing package uid.");
  }

  if (!context->timeline) {
    return base::ErrStatus("ReduceFrameCookies: missing timeline.");
  }

  // Even though it is rare, it is possible for there to be no SurfaceFlinger
  // frame cookies. Even through the main path handles this, we use this early
  // exit to document this edge case.
  if (context->global_frame_cookies.empty()) {
    return base::OkStatus();
  }

  const auto* timeline = context->timeline.get();
  auto uid = context->package_uid.value();

  auto& package_frame_cookies = context->package_frame_cookies;

  // Filter the global cookies down to cookies that belong to the target package
  // (uid).
  for (const auto& cookie : context->global_frame_cookies) {
    auto cookie_slice = timeline->Search(cookie.ts, cookie.pid);

    if (cookie_slice.uid == uid) {
      package_frame_cookies.insert(cookie.cookie);
    }
  }

  return base::OkStatus();
}

bool FilterFrameEvents::KeepField(const Context& context,
                                  const protozero::Field& field) const {
  // If this field is not a timeline event, then this primitive has no reason to
  // reject this field.
  //
  // If it is a timeline event, the event's cookie must be in the package's
  // cookies.
  if (field.id() !=
      protos::pbzero::TracePacket::kFrameTimelineEventFieldNumber) {
    return true;
  }

  protozero::ProtoDecoder timeline_event_decoder(field.as_bytes());

  auto handlers = {
      kActualDisplayFrameStart,
      kActualSurfaceFrameStart,
      kExpectedDisplayFrameStart,
      kExpectedSurfaceFrameStart,
      kFrameEnd,
  };

  const auto& cookies = context.package_frame_cookies;

  for (const auto& handler : handlers) {
    auto event = timeline_event_decoder.FindField(handler.id);

    if (!event.valid()) {
      continue;
    }

    protozero::ProtoDecoder event_decoder(event.as_bytes());

    auto cookie = event_decoder.FindField(handler.cookie);

    if (cookie.valid() && cookies.count(cookie.as_int64())) {
      return true;
    }
  }

  return false;
}

}  // namespace perfetto::trace_redaction
