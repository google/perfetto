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

#include "src/trace_processor/db/view.h"
#include "src/trace_processor/db/view_unittest_py.h"
#include "src/trace_processor/views/macros.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace tables {

ViewThreadTable::~ViewThreadTable() = default;
ViewTrackTable::~ViewTrackTable() = default;
ViewThreadTrackTable::~ViewThreadTrackTable() = default;
ViewEventTable::~ViewEventTable() = default;
ViewSliceTable::~ViewSliceTable() = default;

namespace {

template <typename ViewSubclass>
class AbstractViewTest : public ::testing::Test {
 protected:
  using ColIdx = typename ViewSubclass::ColumnIndex;
  using QueryResult = typename ViewSubclass::QueryResult;

  virtual ~AbstractViewTest() = default;

  QueryResult Query(const std::vector<Constraint>& cs = {},
                    const std::vector<Order>& ob = {}) {
    return Query(cs, ob, AllColsUsed(view()));
  }
  QueryResult QueryUsingCols(const std::vector<uint32_t>& cols_used) {
    return Query({}, {}, cols_used);
  }
  QueryResult Query(const std::vector<Constraint>& cs,
                    const std::vector<Order>& ob,
                    const std::vector<uint32_t>& cols_used) {
    return view().Query(cs, ob, IvToBv(view(), cols_used));
  }

  StringPool::Id Intern(const char* ptr) { return pool_.InternString(ptr); }

  virtual ViewSubclass& view() = 0;

  StringPool pool_;

 private:
  std::vector<uint32_t> AllColsUsed(const View& v) {
    std::vector<uint32_t> used(v.GetColumnCount());
    std::iota(used.begin(), used.end(), 0);
    return used;
  }

  BitVector IvToBv(const View& v, const std::vector<uint32_t>& cols_used) {
    BitVector bv(v.GetColumnCount());
    for (uint32_t col : cols_used) {
      bv.Set(col);
    }
    return bv;
  }
};

#define PERFETTO_TP_EVENT_VIEW_DEF(NAME, FROM, JOIN, COL, _)               \
  NAME(ViewEventView, "event_view")                                        \
  FROM(ViewEventTable, event)                                              \
  JOIN(ViewTrackTable, track, id, event, track_id, View::kIdAlwaysPresent) \
  COL(id, event, id)                                                       \
  COL(ts, event, ts)                                                       \
  COL(track_id, event, track_id)                                           \
  COL(track_name, track, name)
PERFETTO_TP_DECLARE_VIEW(PERFETTO_TP_EVENT_VIEW_DEF);
PERFETTO_TP_DEFINE_VIEW(ViewEventView);

class EventViewTest : public AbstractViewTest<ViewEventView> {
 protected:
  EventViewTest() {
    t1_id_ = track_.Insert({/* name */ Intern("foo")}).id;
    t2_id_ = track_.Insert({/* name */ Intern("bar")}).id;

    event_table_.Insert({/* ts */ 100, t1_id_});
    event_table_.Insert({/* ts */ 101, t2_id_});
    event_table_.Insert({/* ts */ 102, t1_id_});
  }

  virtual ViewEventView& view() override { return event_view_; }

  ViewTrackTable::Id t1_id_;
  ViewTrackTable::Id t2_id_;

 private:
  ViewEventTable event_table_{&pool_};
  ViewTrackTable track_{&pool_};
  ViewEventView event_view_{&event_table_, &track_};
};

TEST_F(EventViewTest, UnusedColumnsAreDummy) {
  ViewEventView::QueryResult result = QueryUsingCols({ColIdx::track_name});
  ASSERT_TRUE(result.columns()[ColIdx::id].IsDummy());
  ASSERT_TRUE(result.columns()[ColIdx::ts].IsDummy());
  ASSERT_FALSE(result.columns()[ColIdx::track_name].IsDummy());
}

TEST_F(EventViewTest, Iterate) {
  ViewEventView::QueryResult result = Query();
  auto it = result.IterateRows();
  ASSERT_TRUE(it);
  ASSERT_EQ(it.row_number().row_number(), 0u);
  ASSERT_EQ(it.ts(), 100);
  ASSERT_EQ(it.track_name(), Intern("foo"));
  ASSERT_EQ(it.track_id(), t1_id_);

  ASSERT_TRUE(++it);
  ASSERT_EQ(it.row_number().row_number(), 1u);
  ASSERT_EQ(it.ts(), 101);
  ASSERT_EQ(it.track_name(), Intern("bar"));
  ASSERT_EQ(it.track_id(), t2_id_);

  ASSERT_TRUE(++it);
  ASSERT_EQ(it.row_number().row_number(), 2u);
  ASSERT_EQ(it.ts(), 102);
  ASSERT_EQ(it.track_name(), Intern("foo"));
  ASSERT_EQ(it.track_id(), t1_id_);

  ASSERT_FALSE(++it);
}

TEST_F(EventViewTest, FilterEventEmpty) {
  ViewEventView::QueryResult result = Query({view().ts().eq(0)});
  auto it = result.IterateRows();
  ASSERT_FALSE(it);
}

TEST_F(EventViewTest, FilterEventNoUseTrack) {
  ViewEventView::QueryResult result =
      Query({view().ts().eq(100)}, {}, {ColIdx::ts});
  auto it = result.IterateRows();
  ASSERT_TRUE(it);
  ASSERT_EQ(it.ts(), 100);

  ASSERT_FALSE(++it);
}

TEST_F(EventViewTest, FilterEventUseTrack) {
  ViewEventView::QueryResult result =
      Query({view().ts().eq(100)}, {},
            {ColIdx::ts, ColIdx::track_name, ColIdx::track_id});
  auto it = result.IterateRows();
  ASSERT_TRUE(it);
  ASSERT_EQ(it.ts(), 100);
  ASSERT_EQ(it.track_name(), Intern("foo"));
  ASSERT_EQ(it.track_id(), t1_id_);

  ASSERT_FALSE(++it);
}

TEST_F(EventViewTest, FilterTrackEmpty) {
  ViewEventView::QueryResult result = Query({view().track_id().eq(102398)});
  auto it = result.IterateRows();
  ASSERT_FALSE(it);
}

TEST_F(EventViewTest, FilterTrackNoUseEvent) {
  ViewEventView::QueryResult result =
      Query({view().track_name().eq("foo")}, {},
            {ColIdx::track_name, ColIdx::track_id});
  auto it = result.IterateRows();
  ASSERT_TRUE(it);
  ASSERT_EQ(it.track_id(), t1_id_);
  ASSERT_EQ(it.track_name(), Intern("foo"));

  ASSERT_TRUE(++it);
  ASSERT_EQ(it.track_id(), t1_id_);
  ASSERT_EQ(it.track_name(), Intern("foo"));

  ASSERT_FALSE(++it);
}

TEST_F(EventViewTest, FilterTrackUseEvent) {
  ViewEventView::QueryResult result =
      Query({view().track_id().eq(t1_id_.value)}, {},
            {ColIdx::ts, ColIdx::track_name, ColIdx::track_id});
  auto it = result.IterateRows();
  ASSERT_TRUE(it);
  ASSERT_EQ(it.ts(), 100);
  ASSERT_EQ(it.track_name(), Intern("foo"));
  ASSERT_EQ(it.track_id(), t1_id_);

  ASSERT_TRUE(++it);
  ASSERT_EQ(it.ts(), 102);
  ASSERT_EQ(it.track_name(), Intern("foo"));
  ASSERT_EQ(it.track_id(), t1_id_);

  ASSERT_FALSE(++it);
}

#define PERFETTO_TP_THREAD_EVENT_VIEW_DEF(NAME, FROM, JOIN, COL, _)      \
  NAME(ViewThreadEventView, "thread_event_view")                         \
  FROM(ViewEventTable, event)                                            \
  JOIN(ViewThreadTrackTable, track, id, event, track_id, View::kNoFlag)  \
  JOIN(ViewThreadTable, thread, id, track, utid, View::kIdAlwaysPresent) \
  COL(id, event, id)                                                     \
  COL(ts, event, ts)                                                     \
  COL(track_id, track, id)                                               \
  COL(track_name, track, name)                                           \
  COL(utid, track, utid)                                                 \
  COL(thread_name, thread, name)
PERFETTO_TP_DECLARE_VIEW(PERFETTO_TP_THREAD_EVENT_VIEW_DEF);
PERFETTO_TP_DEFINE_VIEW(ViewThreadEventView);

class ThreadEventViewTest : public AbstractViewTest<ViewThreadEventView> {
 protected:
  ThreadEventViewTest() {
    th1_id_ = thread_.Insert({Intern("th1"), 1}).id;
    th2_id_ = thread_.Insert({Intern("th2"), 2}).id;

    t1_id_ = track_.Insert({/* name */ Intern("t1")}).id;
    t2_id_ = track_.Insert({/* name */ Intern("t2")}).id;
    t3_id_ = thread_track_.Insert({/* name */ Intern("t3"), th2_id_}).id;
    t4_id_ = thread_track_.Insert({/* name */ Intern("t4"), th1_id_}).id;
    t5_id_ = thread_track_.Insert({/* name */ Intern("t5"), th2_id_}).id;
    t6_id_ = track_.Insert({/* name */ Intern("t6")}).id;

    event_table_.Insert({/* ts */ 100, t1_id_});
    event_table_.Insert({/* ts */ 101, t2_id_});
    event_table_.Insert({/* ts */ 102, t3_id_});
    event_table_.Insert({/* ts */ 103, t5_id_});
    event_table_.Insert({/* ts */ 104, t4_id_});
    event_table_.Insert({/* ts */ 105, t5_id_});
    event_table_.Insert({/* ts */ 106, t1_id_});
    event_table_.Insert({/* ts */ 107, t4_id_});
  }

  virtual ViewThreadEventView& view() override { return event_view_; }

  ViewThreadTable::Id th1_id_;
  ViewThreadTable::Id th2_id_;

  ViewTrackTable::Id t1_id_;
  ViewTrackTable::Id t2_id_;
  ViewTrackTable::Id t3_id_;
  ViewTrackTable::Id t4_id_;
  ViewTrackTable::Id t5_id_;
  ViewTrackTable::Id t6_id_;

 private:
  ViewEventTable event_table_{&pool_};
  ViewTrackTable track_{&pool_};
  ViewThreadTrackTable thread_track_{&pool_, &track_};
  ViewThreadTable thread_{&pool_};
  ViewThreadEventView event_view_{&event_table_, &thread_track_, &thread_};
};

TEST_F(ThreadEventViewTest, Iterate) {
  auto result = Query();
  auto it = result.IterateRows();

  ASSERT_TRUE(it);
  ASSERT_EQ(it.ts(), 102);
  ASSERT_EQ(it.track_id(), t3_id_);
  ASSERT_EQ(it.track_name(), Intern("t3"));
  ASSERT_EQ(it.utid(), th2_id_);
  ASSERT_EQ(it.thread_name(), Intern("th2"));

  ASSERT_TRUE(++it);
  ASSERT_EQ(it.ts(), 103);
  ASSERT_EQ(it.track_name(), Intern("t5"));
  ASSERT_EQ(it.track_id(), t5_id_);
  ASSERT_EQ(it.utid(), th2_id_);
  ASSERT_EQ(it.thread_name(), Intern("th2"));

  ASSERT_TRUE(++it);
  ASSERT_EQ(it.ts(), 104);
  ASSERT_EQ(it.track_id(), t4_id_);
  ASSERT_EQ(it.track_name(), Intern("t4"));
  ASSERT_EQ(it.utid(), th1_id_);
  ASSERT_EQ(it.thread_name(), Intern("th1"));

  ASSERT_TRUE(++it);
  ASSERT_EQ(it.ts(), 105);
  ASSERT_EQ(it.track_id(), t5_id_);
  ASSERT_EQ(it.track_name(), Intern("t5"));
  ASSERT_EQ(it.utid(), th2_id_);
  ASSERT_EQ(it.thread_name(), Intern("th2"));

  ASSERT_TRUE(++it);
  ASSERT_EQ(it.ts(), 107);
  ASSERT_EQ(it.track_id(), t4_id_);
  ASSERT_EQ(it.track_name(), Intern("t4"));
  ASSERT_EQ(it.utid(), th1_id_);
  ASSERT_EQ(it.thread_name(), Intern("th1"));

  ASSERT_FALSE(++it);
}

TEST_F(ThreadEventViewTest, FilterEventUseTrackAndThread) {
  auto result =
      Query({view().ts().ge(105)}, {},
            {ColIdx::ts, ColIdx::track_id, ColIdx::utid, ColIdx::thread_name});
  auto it = result.IterateRows();

  ASSERT_TRUE(it);
  ASSERT_EQ(it.ts(), 105);
  ASSERT_EQ(it.track_id(), t5_id_);
  ASSERT_EQ(it.utid(), th2_id_);
  ASSERT_EQ(it.thread_name(), Intern("th2"));

  ASSERT_TRUE(++it);
  ASSERT_EQ(it.ts(), 107);
  ASSERT_EQ(it.track_id(), t4_id_);
  ASSERT_EQ(it.utid(), th1_id_);
  ASSERT_EQ(it.thread_name(), Intern("th1"));

  ASSERT_FALSE(++it);
}

TEST_F(ThreadEventViewTest, FilterEventUseThreadNoUseTrack) {
  auto result = Query({view().ts().ge(103), view().ts().le(105)}, {},
                      {ColIdx::ts, ColIdx::thread_name});
  auto it = result.IterateRows();

  ASSERT_TRUE(it);
  ASSERT_EQ(it.ts(), 103);
  ASSERT_EQ(it.thread_name(), Intern("th2"));

  ASSERT_TRUE(++it);
  ASSERT_EQ(it.ts(), 104);
  ASSERT_EQ(it.thread_name(), Intern("th1"));

  ASSERT_TRUE(++it);
  ASSERT_EQ(it.ts(), 105);
  ASSERT_EQ(it.thread_name(), Intern("th2"));

  ASSERT_FALSE(++it);
}

TEST_F(ThreadEventViewTest, FilterTrackUseEventNoUseThread) {
  auto result = Query({view().track_id().eq(t4_id_.value)}, {},
                      {ColIdx::ts, ColIdx::track_id});
  auto it = result.IterateRows();

  ASSERT_TRUE(it);
  ASSERT_EQ(it.ts(), 104);
  ASSERT_EQ(it.track_id(), t4_id_);

  ASSERT_TRUE(++it);
  ASSERT_EQ(it.ts(), 107);
  ASSERT_EQ(it.track_id(), t4_id_);

  ASSERT_FALSE(++it);
}

TEST_F(ThreadEventViewTest, FilterEventAndTrack) {
  auto result = Query({view().ts().ge(103), view().track_name().eq("t5")}, {},
                      {ColIdx::ts, ColIdx::track_name});
  auto it = result.IterateRows();

  ASSERT_TRUE(it);
  ASSERT_EQ(it.ts(), 103);
  ASSERT_EQ(it.track_name(), Intern("t5"));

  ASSERT_TRUE(++it);
  ASSERT_EQ(it.ts(), 105);
  ASSERT_EQ(it.track_name(), Intern("t5"));

  ASSERT_FALSE(++it);
}

TEST_F(ThreadEventViewTest, FilterEventAndThread) {
  auto result = Query({view().ts().ge(103), view().thread_name().eq("th1")}, {},
                      {ColIdx::ts, ColIdx::thread_name});
  auto it = result.IterateRows();

  ASSERT_TRUE(it);
  ASSERT_EQ(it.ts(), 104);
  ASSERT_EQ(it.thread_name(), Intern("th1"));

  ASSERT_TRUE(++it);
  ASSERT_EQ(it.ts(), 107);
  ASSERT_EQ(it.thread_name(), Intern("th1"));

  ASSERT_FALSE(++it);
}

#define PERFETTO_TP_THREAD_SLICE_VIEW_DEF(NAME, FROM, JOIN, COL, _)     \
  NAME(ViewThreadSliceView, "thread_slice_view")                        \
  COL(id, slice, id)                                                    \
  COL(ts, slice, ts)                                                    \
  COL(name, slice, name)                                                \
  COL(track_id, slice, track_id)                                        \
  COL(track_name, track, name)                                          \
  COL(utid, thread, id)                                                 \
  COL(thread_name, thread, name)                                        \
  FROM(ViewSliceTable, slice)                                           \
  JOIN(ViewThreadTrackTable, track, id, slice, track_id, View::kNoFlag) \
  JOIN(ViewThreadTable, thread, id, track, utid, View::kIdAlwaysPresent)
PERFETTO_TP_DECLARE_VIEW(PERFETTO_TP_THREAD_SLICE_VIEW_DEF);
PERFETTO_TP_DEFINE_VIEW(ViewThreadSliceView);

class ThreadSliceViewTest : public AbstractViewTest<ViewThreadSliceView> {
 protected:
  ThreadSliceViewTest() {
    th1_id_ = thread_.Insert({Intern("th1"), 1}).id;
    th2_id_ = thread_.Insert({Intern("th2"), 2}).id;

    t1_id_ = track_.Insert({/* name */ Intern("t1")}).id;
    t2_id_ = track_.Insert({/* name */ Intern("t2")}).id;
    t3_id_ = thread_track_.Insert({/* name */ Intern("t3"), th2_id_}).id;
    t4_id_ = thread_track_.Insert({/* name */ Intern("t4"), th1_id_}).id;
    t5_id_ = thread_track_.Insert({/* name */ Intern("t5"), th2_id_}).id;
    t6_id_ = track_.Insert({/* name */ Intern("t6")}).id;

    event_.Insert({/* ts */ 100, t1_id_});
    event_.Insert({/* ts */ 101, t2_id_});
    slice_table_.Insert({/* ts */ 102, t3_id_, Intern("ts102")});
    slice_table_.Insert({/* ts */ 103, t5_id_, Intern("ts103")});
    slice_table_.Insert({/* ts */ 104, t4_id_, Intern("ts104")});
    event_.Insert({/* ts */ 105, t5_id_});
    slice_table_.Insert({/* ts */ 106, t1_id_, Intern("ts106")});
    slice_table_.Insert({/* ts */ 107, t4_id_, Intern("ts107")});
  }

  ViewThreadSliceView& view() override { return slice_view_; }

  ViewThreadTable::Id th1_id_;
  ViewThreadTable::Id th2_id_;

  ViewTrackTable::Id t1_id_;
  ViewTrackTable::Id t2_id_;
  ViewTrackTable::Id t3_id_;
  ViewTrackTable::Id t4_id_;
  ViewTrackTable::Id t5_id_;
  ViewTrackTable::Id t6_id_;

 private:
  ViewEventTable event_{&pool_};
  ViewSliceTable slice_table_{&pool_, &event_};
  ViewTrackTable track_{&pool_};
  ViewThreadTrackTable thread_track_{&pool_, &track_};
  ViewThreadTable thread_{&pool_};
  ViewThreadSliceView slice_view_{&slice_table_, &thread_track_, &thread_};
};

TEST_F(ThreadSliceViewTest, Iterate) {
  auto result = Query();
  auto it = result.IterateRows();

  ASSERT_TRUE(it);
  ASSERT_EQ(it.ts(), 102);
  ASSERT_EQ(it.track_id(), t3_id_);
  ASSERT_EQ(it.track_name(), Intern("t3"));
  ASSERT_EQ(it.utid(), th2_id_);
  ASSERT_EQ(it.thread_name(), Intern("th2"));

  ASSERT_TRUE(++it);
  ASSERT_EQ(it.ts(), 103);
  ASSERT_EQ(it.track_name(), Intern("t5"));
  ASSERT_EQ(it.track_id(), t5_id_);
  ASSERT_EQ(it.utid(), th2_id_);
  ASSERT_EQ(it.thread_name(), Intern("th2"));

  ASSERT_TRUE(++it);
  ASSERT_EQ(it.ts(), 104);
  ASSERT_EQ(it.track_id(), t4_id_);
  ASSERT_EQ(it.track_name(), Intern("t4"));
  ASSERT_EQ(it.utid(), th1_id_);
  ASSERT_EQ(it.thread_name(), Intern("th1"));

  ASSERT_TRUE(++it);
  ASSERT_EQ(it.ts(), 107);
  ASSERT_EQ(it.track_id(), t4_id_);
  ASSERT_EQ(it.track_name(), Intern("t4"));
  ASSERT_EQ(it.utid(), th1_id_);
  ASSERT_EQ(it.thread_name(), Intern("th1"));

  ASSERT_FALSE(++it);
}

TEST_F(ThreadSliceViewTest, FilterAll) {
  auto result = Query({view().ts().le(106), view().track_id().le(t4_id_.value),
                       view().thread_name().eq("th2")},
                      {}, {ColIdx::ts, ColIdx::track_id, ColIdx::thread_name});
  auto it = result.IterateRows();

  ASSERT_TRUE(it);
  ASSERT_EQ(it.ts(), 102);
  ASSERT_EQ(it.track_id(), t3_id_);
  ASSERT_EQ(it.thread_name(), Intern("th2"));

  ASSERT_FALSE(++it);
}

TEST_F(ThreadSliceViewTest, FilterEventAndTrack) {
  auto result = Query({view().ts().le(106), view().track_id().le(t4_id_.value)},
                      {}, {ColIdx::ts, ColIdx::track_id});
  auto it = result.IterateRows();

  ASSERT_TRUE(it);
  ASSERT_EQ(it.ts(), 102);
  ASSERT_EQ(it.track_id(), t3_id_);

  ASSERT_TRUE(++it);
  ASSERT_EQ(it.ts(), 104);
  ASSERT_EQ(it.track_id(), t4_id_);

  ASSERT_FALSE(++it);
}

TEST_F(ThreadSliceViewTest, Sort) {
  auto result =
      Query({}, {view().track_id().ascending(), view().ts().descending()},
            {ColIdx::track_id, ColIdx::ts});
  auto it = result.IterateRows();

  ASSERT_TRUE(it);
  ASSERT_EQ(it.ts(), 102);
  ASSERT_EQ(it.track_id(), t3_id_);

  ASSERT_TRUE(++it);
  ASSERT_EQ(it.ts(), 107);
  ASSERT_EQ(it.track_id(), t4_id_);

  ASSERT_TRUE(++it);
  ASSERT_EQ(it.ts(), 104);
  ASSERT_EQ(it.track_id(), t4_id_);

  ASSERT_TRUE(++it);
  ASSERT_EQ(it.ts(), 103);
  ASSERT_EQ(it.track_id(), t5_id_);

  ASSERT_FALSE(++it);
}

TEST_F(ThreadSliceViewTest, FilterAndSort) {
  auto result = Query({view().track_id().lt(t5_id_.value)},
                      {view().track_id().ascending(), view().ts().descending()},
                      {ColIdx::track_id, ColIdx::ts, ColIdx::thread_name});
  auto it = result.IterateRows();

  ASSERT_TRUE(it);
  ASSERT_EQ(it.ts(), 102);
  ASSERT_EQ(it.track_id(), t3_id_);
  ASSERT_EQ(it.thread_name(), Intern("th2"));

  ASSERT_TRUE(++it);
  ASSERT_EQ(it.ts(), 107);
  ASSERT_EQ(it.track_id(), t4_id_);
  ASSERT_EQ(it.thread_name(), Intern("th1"));

  ASSERT_TRUE(++it);
  ASSERT_EQ(it.ts(), 104);
  ASSERT_EQ(it.track_id(), t4_id_);
  ASSERT_EQ(it.thread_name(), Intern("th1"));

  ASSERT_FALSE(++it);
}

}  // namespace
}  // namespace tables
}  // namespace trace_processor
}  // namespace perfetto
