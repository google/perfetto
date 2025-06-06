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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_WINSCOPE_RECT_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_WINSCOPE_RECT_H_

#include <algorithm>
#include "protos/perfetto/trace/android/graphics/rect.pbzero.h"
#include "protos/perfetto/trace/android/surfaceflinger_layers.pbzero.h"
#include "src/trace_processor/tables/winscope_tables_py.h"
#include "src/trace_processor/types/destructible.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

static bool isFloatEqual(double a, double b) {
  return std::abs(a - b) < 0.000001;
}

static bool isFloatClose(double a, double b) {
  return std::abs(a - b) < 0.01;
}

struct WinscopeRect {
  double x = 0;
  double y = 0;
  double w = 0;
  double h = 0;

  static const WinscopeRect makeRect(
      const protos::pbzero::RectProto::Decoder& rect) {
    return WinscopeRect::makeRect(rect.has_left() ? rect.left() : 0,
                                  rect.has_top() ? rect.top() : 0,
                                  rect.has_right() ? rect.right() : 0,
                                  rect.has_bottom() ? rect.bottom() : 0);
  }

  static const WinscopeRect makeRect(
      const protos::pbzero::FloatRectProto::Decoder& rect) {
    return WinscopeRect::makeRect(
        rect.has_left() ? static_cast<double>(rect.left()) : 0,
        rect.has_top() ? static_cast<double>(rect.top()) : 0,
        rect.has_right() ? static_cast<double>(rect.right()) : 0,
        rect.has_bottom() ? static_cast<double>(rect.bottom()) : 0);
  }

  static const WinscopeRect makeRect(double left,
                                     double top,
                                     double right,
                                     double bottom) {
    return WinscopeRect{left, top, right - left, bottom - top};
  }

  bool isEmpty() const {
    const bool nullValuePresent = isFloatEqual(x, -1) || isFloatEqual(y, -1) ||
                                  isFloatEqual(x + w, -1) ||
                                  isFloatEqual(y + h, -1);
    const bool nullHeightOrWidth = w <= 0 || h <= 0;
    return nullValuePresent || nullHeightOrWidth;
  }

  WinscopeRect cropRect(const WinscopeRect& other) {
    const auto max_left = std::max(x, other.x);
    const auto min_right = std::min(x + w, other.x + other.w);
    const auto max_top = std::max(y, other.y);
    const auto min_bottom = std::min(y + h, other.y + other.h);
    return WinscopeRect{max_left, max_top, min_right - max_left,
                        min_bottom - max_top};
  }

  bool containsRect(const WinscopeRect& other) const {
    return (w > 0 && h > 0 && x <= other.x && y <= other.y &&
            (x + w >= other.x + other.w) && (y + h >= other.y + other.h));
  }

  bool intersectsRect(const WinscopeRect& other) const {
    if (x < other.x + other.w && other.x < x + w && y <= other.y + other.h &&
        other.y <= y + h) {
      auto new_x = x;
      auto new_y = y;
      auto new_w = w;
      auto new_h = h;

      if (x < other.x) {
        new_x = other.x;
      }
      if (y < other.y) {
        new_y = other.y;
      }
      if (x + w > other.x + other.w) {
        new_w = other.w;
      }
      if (y + h > other.y + other.h) {
        new_h = other.h;
      }

      return !WinscopeRect{new_x, new_y, new_w, new_h}.isEmpty();
    }
    return false;
  }

  bool operator==(const WinscopeRect& other) const {
    return isFloatEqual(x, other.x) && isFloatEqual(y, other.y) &&
           isFloatEqual(w, other.w) && isFloatEqual(h, other.h);
  }

  bool isAlmostEqual(const WinscopeRect& other) const {
    return (isFloatClose(x, other.x) && isFloatClose(y, other.y) &&
            isFloatClose(w, other.w) && isFloatClose(h, other.h));
  }
};

struct Point {
  double x;
  double y;
};

struct Size {
  double w;
  double h;
};

struct Region {
  std::vector<WinscopeRect> rects;
};

class WinscopeRectTracker : public Destructible {
 public:
  explicit WinscopeRectTracker(TraceProcessorContext*);
  virtual ~WinscopeRectTracker() override;
  TraceProcessorContext* context_;

  static WinscopeRectTracker* GetOrCreate(TraceProcessorContext* context) {
    if (!context->winscope_rect_tracker) {
      context->winscope_rect_tracker.reset(new WinscopeRectTracker(context));
    }
    return static_cast<WinscopeRectTracker*>(
        context->winscope_rect_tracker.get());
  }

  tables::WinscopeRectTable::Id* GetOrInsertRow(const WinscopeRect& rect);

 private:
  struct Row {
    tables::WinscopeRectTable::Id row_id;
    const WinscopeRect rect;
  };
  std::vector<Row> rows_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_WINSCOPE_RECT_H_
