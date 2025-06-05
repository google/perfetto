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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_WINSCOPE_TRANSFORM_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_WINSCOPE_TRANSFORM_H_

#include <algorithm>
#include "protos/perfetto/trace/android/graphics/rect.pbzero.h"
#include "protos/perfetto/trace/android/surfaceflinger_layers.pbzero.h"
#include "src/trace_processor/importers/proto/winscope/winscope_rect.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/winscope_tables_py.h"
#include "src/trace_processor/types/destructible.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

struct TransformMatrix {
  double dsdx = 1;
  double dtdx = 0;
  double tx = 0;
  double dtdy = 0;
  double dsdy = 1;
  double ty = 0;

  bool operator==(const TransformMatrix& other) const {
    return isFloatEqual(dsdx, other.dsdx) && isFloatEqual(dsdy, other.dsdy) &&
           isFloatEqual(dtdx, other.dtdx) && isFloatEqual(dtdy, other.dtdy) &&
           isFloatEqual(tx, other.tx) && isFloatEqual(ty, other.ty);
  }

  const Point transformPoint(Point point) const {
    return {
        dsdx * point.x + dtdx * point.y + tx,
        dtdy * point.x + dsdy * point.y + ty,
    };
  }

  const WinscopeRect transformRect(const WinscopeRect& r) const {
    const auto lt_prime = transformPoint({r.x, r.y});
    const auto rb_prime = transformPoint({r.x + r.w, r.y + r.h});
    const auto x = std::min(lt_prime.x, rb_prime.x);
    const auto y = std::min(lt_prime.y, rb_prime.y);
    return WinscopeRect{
        x,
        y,
        std::max(lt_prime.x, rb_prime.x) - x,
        std::max(lt_prime.y, rb_prime.y) - y,
    };
  }

  const Region transformRegion(Region region) const {
    std::vector<WinscopeRect> rects;
    for (const auto& rect : region.rects) {
      rects.push_back(transformRect(rect));
    }
    return Region{rects};
  }

  const TransformMatrix inverse() const {
    const auto ident = 1.0 / det();
    TransformMatrix inverse = TransformMatrix{
        dsdy * ident, -dtdx * ident, 0, -dtdy * ident, dsdx * ident, 0,
    };
    auto t = inverse.transformPoint(Point{
        -tx,
        -ty,
    });
    inverse.tx = t.x;
    inverse.ty = t.y;
    return inverse;
  }

  bool isValid() const { return !isFloatEqual(dsdx * dsdy, dtdx * dtdy); }

 private:
  double det() const { return dsdx * dsdy - dtdx * dtdy; }
};

class WinscopeTransformTracker : public Destructible {
 public:
  explicit WinscopeTransformTracker(TraceProcessorContext*);
  virtual ~WinscopeTransformTracker() override;

  static WinscopeTransformTracker* GetOrCreate(TraceProcessorContext* context) {
    if (!context->winscope_transform_tracker) {
      auto tracker = new WinscopeTransformTracker(context);
      tracker->GetOrInsertRow(TransformMatrix{});
      context->winscope_transform_tracker.reset(tracker);
    }
    return static_cast<WinscopeTransformTracker*>(
        context->winscope_transform_tracker.get());
  }

  TraceProcessorContext* context_;

  tables::WinscopeTransformTable::Id* GetOrInsertRow(
      const TransformMatrix& matrix);

 private:
  struct Row {
    tables::WinscopeTransformTable::Id row_id;
    const TransformMatrix matrix;
  };
  std::vector<Row> rows_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_WINSCOPE_TRANSFORM_H_
