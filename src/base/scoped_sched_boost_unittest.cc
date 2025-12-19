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

#include "perfetto/ext/base/scoped_sched_boost.h"

#include "perfetto/base/build_config.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)

#include <sched.h>
#include <sys/resource.h>

#include "perfetto/base/thread_utils.h"
#include "test/gtest_and_gmock.h"
#include "test/status_matchers.h"

using testing::_;
using testing::ElementsAre;
using testing::Eq;
using testing::NiceMock;
using testing::Return;

namespace perfetto::base {

// For ASSERT_EQ()
inline bool operator==(const SchedOsHooks::SchedOsConfig& lhs,
                       const SchedOsHooks::SchedOsConfig& rhs) {
  return std::tie(lhs.policy, lhs.rt_prio, lhs.nice) ==
         std::tie(rhs.policy, rhs.rt_prio, rhs.nice);
}

// For ASSERT_EQ()
inline std::ostream& operator<<(std::ostream& os,
                                const SchedOsHooks::SchedOsConfig& s) {
  return os << "SchedOsConfig{policy: " << s.policy << ", prio: " << s.rt_prio
            << ", nice: " << s.nice << "}";
}

// For ASSERT_EQ()
inline std::ostream& operator<<(std::ostream& os,
                                const SchedPolicyAndPrio& spp) {
  return os << "SchedPolicyAndPrio{policy: "
            << (spp.policy == SchedPolicyAndPrio::Policy::kSchedOther
                    ? "SCHED_OTHER"
                    : "SCHED_FIFO")
            << ", prio: " << spp.prio << "}";
}

namespace {

class MockSchedOsHooks : public SchedOsHooks {
 public:
  explicit MockSchedOsHooks(SchedOsConfig init_config)
      : current_config(init_config) {
    ON_CALL(*this, GetCurrentSchedConfig()).WillByDefault([&] {
      return current_config;
    });
    ON_CALL(*this, SetSchedConfig).WillByDefault([&](const SchedOsConfig& arg) {
      current_config = arg;
      return OkStatus();
    });
  }
  MOCK_METHOD(base::Status, SetSchedConfig, (const SchedOsConfig&), (override));
  MOCK_METHOD(base::StatusOr<base::SchedOsHooks::SchedOsConfig>,
              GetCurrentSchedConfig,
              (),
              (const, override));

  ~MockSchedOsHooks() override = default;
  SchedOsConfig current_config;
};

class ScopedSchedBoostTest : public testing::Test {
 public:
  void SetUp() override { ScopedSchedBoost::ResetForTesting(&sched_hooks_); }

  void TearDown() override {
    ASSERT_EQ(sched_hooks_.current_config, kInitSchedOsConfig);
  }

  const SchedOsHooks::SchedOsConfig kInitSchedOsConfig{SCHED_OTHER, 0, 0};
  NiceMock<MockSchedOsHooks> sched_hooks_{kInitSchedOsConfig};
};

TEST_F(ScopedSchedBoostTest, SchedPolicyAndPrioOrder) {
  SchedPolicyAndPrio fifo1{SchedPolicyAndPrio::Policy::kSchedFifo, 1};
  SchedPolicyAndPrio fifo99{SchedPolicyAndPrio::Policy::kSchedFifo, 99};
  SchedPolicyAndPrio other0{SchedPolicyAndPrio::Policy::kSchedOther, 0};
  SchedPolicyAndPrio other1{SchedPolicyAndPrio::Policy::kSchedOther, 1};
  SchedPolicyAndPrio other10{SchedPolicyAndPrio::Policy::kSchedOther, 10};

  std::set sorted_spp{fifo1, fifo99, other0, other1, other10};
  ASSERT_THAT(sorted_spp, ElementsAre(other0, other1, other10, fifo1, fifo99));
}

TEST_F(ScopedSchedBoostTest, ScopeEnterExit) {
  {
    auto boost5 =
        ScopedSchedBoost::Boost({SchedPolicyAndPrio::Policy::kSchedOther, 5});
    ASSERT_OK(boost5);
    ASSERT_THAT(sched_hooks_.current_config,
                Eq(SchedOsHooks::SchedOsConfig{SCHED_OTHER, 0, -5}));
    auto boost3 =
        ScopedSchedBoost::Boost({SchedPolicyAndPrio::Policy::kSchedOther, 3});
    ASSERT_OK(boost3);
    // boost3 is less than boost5, assert we don't change the policy.
    ASSERT_THAT(sched_hooks_.current_config,
                Eq(SchedOsHooks::SchedOsConfig{SCHED_OTHER, 0, -5}));
    {
      auto boost10 = ScopedSchedBoost::Boost(
          {SchedPolicyAndPrio::Policy::kSchedOther, 10});
      ASSERT_OK(boost10);
      ASSERT_THAT(sched_hooks_.current_config,
                  Eq(SchedOsHooks::SchedOsConfig{SCHED_OTHER, 0, -10}));
      {
        auto boost42 = ScopedSchedBoost::Boost(
            {SchedPolicyAndPrio::Policy::kSchedFifo, 42});
        ASSERT_OK(boost42);
        ASSERT_THAT(sched_hooks_.current_config,
                    Eq(SchedOsHooks::SchedOsConfig{SCHED_FIFO, 42, 0}));
        {
          auto boost12 = ScopedSchedBoost::Boost(
              {SchedPolicyAndPrio::Policy::kSchedOther, 12});
          ASSERT_OK(boost12);
          // boost12 is less than boost42, assert we don't change the policy.
          ASSERT_THAT(sched_hooks_.current_config,
                      Eq(SchedOsHooks::SchedOsConfig{SCHED_FIFO, 42, 0}));
        }
        {
          auto boost5_nested = ScopedSchedBoost::Boost(
              {SchedPolicyAndPrio::Policy::kSchedOther, 5});
          ASSERT_OK(boost5_nested);
          // When destroying the boost5_nested, the outer 'boost5' shouldn't be
          // removed.
        }
      }
      ASSERT_THAT(sched_hooks_.current_config,
                  Eq(SchedOsHooks::SchedOsConfig{SCHED_OTHER, 0, -10}));
    }
    ASSERT_THAT(sched_hooks_.current_config,
                Eq(SchedOsHooks::SchedOsConfig{SCHED_OTHER, 0, -5}));
  }
  ASSERT_EQ(sched_hooks_.current_config, kInitSchedOsConfig);
}

TEST_F(ScopedSchedBoostTest, MoveOperation) {
  std::optional<ScopedSchedBoost> moved_boost;
  {
    EXPECT_CALL(sched_hooks_, SetSchedConfig(SchedOsHooks::SchedOsConfig{
                                  SCHED_OTHER, 0, -5}));
    auto boost =
        ScopedSchedBoost::Boost({SchedPolicyAndPrio::Policy::kSchedOther, 5});
    ASSERT_OK(boost);
    ASSERT_THAT(sched_hooks_.current_config,
                Eq(SchedOsHooks::SchedOsConfig{SCHED_OTHER, 0, -5}));
    // Assert we don't call system API when move
    EXPECT_CALL(sched_hooks_, SetSchedConfig(_)).Times(0);
    moved_boost = std::move(boost.value());
  }
  ASSERT_TRUE(moved_boost.has_value());
  ASSERT_THAT(sched_hooks_.current_config,
              Eq(SchedOsHooks::SchedOsConfig{SCHED_OTHER, 0, -5}));
  EXPECT_CALL(sched_hooks_, SetSchedConfig(kInitSchedOsConfig));
  moved_boost.reset();
  ASSERT_EQ(sched_hooks_.current_config, kInitSchedOsConfig);
}

TEST_F(ScopedSchedBoostTest, IgnoreWrongConfig) {
  ON_CALL(sched_hooks_, SetSchedConfig(_))
      .WillByDefault([&](const SchedOsHooks::SchedOsConfig& arg) {
        if (arg.policy == SCHED_FIFO && arg.rt_prio < 1) {
          return ErrStatus("Priority for SCHED_FIFO policy must be >= 1");
        }
        sched_hooks_.current_config = arg;
        return OkStatus();
      });

  auto ok_other_boost = ScopedSchedBoost::Boost(
      SchedPolicyAndPrio{SchedPolicyAndPrio::Policy::kSchedOther, 5});
  ASSERT_OK(ok_other_boost);
  ASSERT_THAT(sched_hooks_.current_config,
              Eq(SchedOsHooks::SchedOsConfig{SCHED_OTHER, 0, -5}));
  {
    auto ok_fifo_boost = ScopedSchedBoost::Boost(
        SchedPolicyAndPrio{SchedPolicyAndPrio::Policy::kSchedFifo, 42});
    ASSERT_OK(ok_fifo_boost);
    std::optional ok_fifo_to_remove(std::move(ok_fifo_boost.value()));
    ASSERT_THAT(sched_hooks_.current_config,
                Eq(SchedOsHooks::SchedOsConfig{SCHED_FIFO, 42, 0}));
    // This isn't the max prio, so it wasn't validated and returns OK
    auto bad_fifo_boost = ScopedSchedBoost::Boost(
        SchedPolicyAndPrio{SchedPolicyAndPrio::Policy::kSchedFifo, 0});
    ASSERT_OK(bad_fifo_boost);

    // After the next line the 'bad_fifo_boost' becomes the max priority.
    // It will be validated, error logged and the priority set to the next
    // valid max priority (ok_other_boost)
    ok_fifo_to_remove.reset();
    ASSERT_THAT(sched_hooks_.current_config,
                Eq(SchedOsHooks::SchedOsConfig{SCHED_OTHER, 0, -5}));
  }

  ASSERT_THAT(sched_hooks_.current_config,
              Eq(SchedOsHooks::SchedOsConfig{SCHED_OTHER, 0, -5}));
}

class ScopedSchedBoostLinuxIntegrationTest : public testing::Test {
  void SetUp() override {
    ScopedSchedBoost::ResetForTesting(SchedOsHooks::GetInstance());
    ASSERT_OK_AND_ASSIGN(initial_config,
                         SchedOsHooks::GetInstance()->GetCurrentSchedConfig());
  }

  void TearDown() override {
    ASSERT_OK(SchedOsHooks::GetInstance()->SetSchedConfig(initial_config));
  }

  SchedOsHooks::SchedOsConfig initial_config{};
};

TEST_F(ScopedSchedBoostLinuxIntegrationTest, LinuxApiCalls) {
  if (geteuid() != 0) {
    GTEST_SKIP() << "LinuxApiCalls requires root";
  }
  {
    auto boost = ScopedSchedBoost::Boost(
        SchedPolicyAndPrio{SchedPolicyAndPrio::Policy::kSchedOther, 5});
    ASSERT_OK(boost);
    SchedOsHooks::SchedOsConfig current{};
    ASSERT_OK_AND_ASSIGN(current,
                         SchedOsHooks::GetInstance()->GetCurrentSchedConfig());
    ASSERT_THAT(current, Eq(SchedOsHooks::SchedOsConfig{SCHED_OTHER, 0, -5}));
    {
      auto boost_rt = ScopedSchedBoost::Boost(
          SchedPolicyAndPrio{SchedPolicyAndPrio::Policy::kSchedFifo, 42});
      ASSERT_OK(boost_rt);
      SchedOsHooks::SchedOsConfig current_rt{};
      ASSERT_OK_AND_ASSIGN(
          current_rt, SchedOsHooks::GetInstance()->GetCurrentSchedConfig());
      ASSERT_THAT(current_rt,
                  Eq(SchedOsHooks::SchedOsConfig{SCHED_FIFO, 42, 0}));
    }

    ASSERT_OK_AND_ASSIGN(current,
                         SchedOsHooks::GetInstance()->GetCurrentSchedConfig());
    ASSERT_THAT(current, Eq(SchedOsHooks::SchedOsConfig{SCHED_OTHER, 0, -5}));
  }
}

TEST_F(ScopedSchedBoostLinuxIntegrationTest, WrongConfig) {
  if (geteuid() != 0) {
    GTEST_SKIP() << "WrongConfig requires root";
  }
  // When using 'Policy::kSchedOther', from man 2 getpriority:
  // Attempts to set a priority outside this range are silently clamped to the
  // range. So we test error reporting only for the 'Policy::kSchedFifo'
  auto boost = ScopedSchedBoost::Boost(
      SchedPolicyAndPrio{SchedPolicyAndPrio::Policy::kSchedFifo, 101});
  ASSERT_STREQ(
      boost.status().c_message(),
      "sched_setscheduler(1, 101) failed (errno: 22, Invalid argument)");
}

TEST_F(ScopedSchedBoostLinuxIntegrationTest, ReturnNoPermission) {
  if (geteuid() == 0) {
    GTEST_SKIP() << "TestNoPermission requires non-root";
  }
  auto boost = ScopedSchedBoost::Boost(
      SchedPolicyAndPrio{SchedPolicyAndPrio::Policy::kSchedFifo, 42});
  ASSERT_STREQ(
      boost.status().c_message(),
      "sched_setscheduler(1, 42) failed (errno: 1, Operation not permitted)");
}

}  // namespace
}  // namespace perfetto::base

#endif
