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
#include "src/trace_processor/tables/macros.h"
#include "src/trace_processor/views/macros.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

#define PERFETTO_TP_TEST_THREAD_TABLE_DEF(NAME, PARENT, C) \
  NAME(TestThreadTable, "thread_table")                    \
  PARENT(PERFETTO_TP_ROOT_TABLE_PARENT_DEF, C)             \
  C(StringPool::Id, name)                                  \
  C(uint32_t, tid)
PERFETTO_TP_TABLE(PERFETTO_TP_TEST_THREAD_TABLE_DEF);

#define PERFETTO_TP_TEST_TRACK_TABLE_DEF(NAME, PARENT, C) \
  NAME(TestTrackTable, "track_table")                     \
  PARENT(PERFETTO_TP_ROOT_TABLE_PARENT_DEF, C)            \
  C(StringPool::Id, name)
PERFETTO_TP_TABLE(PERFETTO_TP_TEST_TRACK_TABLE_DEF);

#define PERFETTO_TP_TEST_THREAD_TRACK_TABLE_DEF(NAME, PARENT, C) \
  NAME(TestThreadTrackTable, "thread_track_table")               \
  PARENT(PERFETTO_TP_TEST_TRACK_TABLE_DEF, C)                    \
  C(TestThreadTable::Id, utid)
PERFETTO_TP_TABLE(PERFETTO_TP_TEST_THREAD_TRACK_TABLE_DEF);

#define PERFETTO_TP_TEST_EVENT_TABLE_DEF(NAME, PARENT, C) \
  NAME(TestEventTable, "event_table")                     \
  PARENT(PERFETTO_TP_ROOT_TABLE_PARENT_DEF, C)            \
  C(int64_t, ts, Column::Flag::kSorted)                   \
  C(TestTrackTable::Id, track_id)
PERFETTO_TP_TABLE(PERFETTO_TP_TEST_EVENT_TABLE_DEF);

#define PERFETTO_TP_TEST_SLICE_TABLE_DEF(NAME, PARENT, C) \
  NAME(TestSliceTable, "slice_table")                     \
  PARENT(PERFETTO_TP_TEST_EVENT_TABLE_DEF, C)             \
  C(StringPool::Id, name)
PERFETTO_TP_TABLE(PERFETTO_TP_TEST_SLICE_TABLE_DEF);

TestThreadTable::~TestThreadTable() = default;
TestTrackTable::~TestTrackTable() = default;
TestThreadTrackTable::~TestThreadTrackTable() = default;
TestEventTable::~TestEventTable() = default;
TestSliceTable::~TestSliceTable() = default;

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
  NAME(TestEventView, "event_view")                                        \
  FROM(TestEventTable, event)                                              \
  JOIN(TestTrackTable, track, id, event, track_id, View::kIdAlwaysPresent) \
  COL(id, event, id)                                                       \
  COL(ts, event, ts)                                                       \
  COL(track_id, event, track_id)                                           \
  COL(track_name, track, name)
PERFETTO_TP_DECLARE_VIEW(PERFETTO_TP_EVENT_VIEW_DEF);
PERFETTO_TP_DEFINE_VIEW(TestEventView);

class EventViewTest : public AbstractViewTest<TestEventView> {
 protected:
  EventViewTest() {
    t1_id_ = track_.Insert({/* name */ Intern("foo")}).id;
    t2_id_ = track_.Insert({/* name */ Intern("bar")}).id;

    event_table_.Insert({/* ts */ 100, t1_id_});
    event_table_.Insert({/* ts */ 101, t2_id_});
    event_table_.Insert({/* ts */ 102, t1_id_});
  }

  virtual TestEventView& view() override { return event_view_; }

  TestTrackTable::Id t1_id_;
  TestTrackTable::Id t2_id_;

 private:
  TestEventTable event_table_{&pool_, nullptr};
  TestTrackTable track_{&pool_, nullptr};
  TestEventView event_view_{&event_table_, &track_};
};

TEST_F(EventViewTest, UnusedColumnsAreDummy) {
  TestEventView::QueryResult result = QueryUsingCols({ColIdx::track_name});
  ASSERT_TRUE(result.columns()[ColIdx::id].IsDummy());
  ASSERT_TRUE(result.columns()[ColIdx::ts].IsDummy());
  ASSERT_FALSE(result.columns()[ColIdx::track_name].IsDummy());
}

TEST_F(EventViewTest, Iterate) {
  TestEventView::QueryResult result = Query();
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
  TestEventView::QueryResult result = Query({view().ts().eq(0)});
  auto it = result.IterateRows();
  ASSERT_FALSE(it);
}

TEST_F(EventViewTest, FilterEventNoUseTrack) {
  TestEventView::QueryResult result =
      Query({view().ts().eq(100)}, {}, {ColIdx::ts});
  auto it = result.IterateRows();
  ASSERT_TRUE(it);
  ASSERT_EQ(it.ts(), 100);

  ASSERT_FALSE(++it);
}

TEST_F(EventViewTest, FilterEventUseTrack) {
  TestEventView::QueryResult result =
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
  TestEventView::QueryResult result = Query({view().track_id().eq(102398)});
  auto it = result.IterateRows();
  ASSERT_FALSE(it);
}

TEST_F(EventViewTest, FilterTrackNoUseEvent) {
  TestEventView::QueryResult result =
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
  TestEventView::QueryResult result =
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
  NAME(TestThreadEventView, "thread_event_view")                         \
  FROM(TestEventTable, event)                                            \
  JOIN(TestThreadTrackTable, track, id, event, track_id, View::kNoFlag)  \
  JOIN(TestThreadTable, thread, id, track, utid, View::kIdAlwaysPresent) \
  COL(id, event, id)                                                     \
  COL(ts, event, ts)                                                     \
  COL(track_id, track, id)                                               \
  COL(track_name, track, name)                                           \
  COL(utid, track, utid)                                                 \
  COL(thread_name, thread, name)
PERFETTO_TP_DECLARE_VIEW(PERFETTO_TP_THREAD_EVENT_VIEW_DEF);
PERFETTO_TP_DEFINE_VIEW(TestThreadEventView);

class ThreadEventViewTest : public AbstractViewTest<TestThreadEventView> {
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

  virtual TestThreadEventView& view() override { return event_view_; }

  TestThreadTable::Id th1_id_;
  TestThreadTable::Id th2_id_;

  TestTrackTable::Id t1_id_;
  TestTrackTable::Id t2_id_;
  TestTrackTable::Id t3_id_;
  TestTrackTable::Id t4_id_;
  TestTrackTable::Id t5_id_;
  TestTrackTable::Id t6_id_;

 private:
  TestEventTable event_table_{&pool_, nullptr};
  TestTrackTable track_{&pool_, nullptr};
  TestThreadTrackTable thread_track_{&pool_, &track_};
  TestThreadTable thread_{&pool_, nullptr};
  TestThreadEventView event_view_{&event_table_, &thread_track_, &thread_};
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
  NAME(TestThreadSliceView, "thread_slice_view")                        \
  COL(id, slice, id)                                                    \
  COL(ts, slice, ts)                                                    \
  COL(name, slice, name)                                                \
  COL(track_id, slice, track_id)                                        \
  COL(track_name, track, name)                                          \
  COL(utid, thread, id)                                                 \
  COL(thread_name, thread, name)                                        \
  FROM(TestSliceTable, slice)                                           \
  JOIN(TestThreadTrackTable, track, id, slice, track_id, View::kNoFlag) \
  JOIN(TestThreadTable, thread, id, track, utid, View::kIdAlwaysPresent)
PERFETTO_TP_DECLARE_VIEW(PERFETTO_TP_THREAD_SLICE_VIEW_DEF);
PERFETTO_TP_DEFINE_VIEW(TestThreadSliceView);

class ThreadSliceViewTest : public AbstractViewTest<TestThreadSliceView> {
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

  TestThreadSliceView& view() override { return slice_view_; }

  TestThreadTable::Id th1_id_;
  TestThreadTable::Id th2_id_;

  TestTrackTable::Id t1_id_;
  TestTrackTable::Id t2_id_;
  TestTrackTable::Id t3_id_;
  TestTrackTable::Id t4_id_;
  TestTrackTable::Id t5_id_;
  TestTrackTable::Id t6_id_;

 private:
  TestEventTable event_{&pool_, nullptr};
  TestSliceTable slice_table_{&pool_, &event_};
  TestTrackTable track_{&pool_, nullptr};
  TestThreadTrackTable thread_track_{&pool_, &track_};
  TestThreadTable thread_{&pool_, nullptr};
  TestThreadSliceView slice_view_{&slice_table_, &thread_track_, &thread_};
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
}  // namespace trace_processor
}  // namespace perfetto
