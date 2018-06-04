#include "src/traced/probes/ftrace/cpu_stats_parser.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "src/traced/probes/ftrace/ftrace_controller.h"

namespace perfetto {
namespace {

TEST(CpuStatsParserTest, DumpCpu) {
  std::string text = R"(entries: 1
overrun: 2
commit overrun: 3
bytes: 4
oldest event ts:     5123.000
now ts:  6123.123
dropped events	 	:7
read events: 8
)";

  FtraceCpuStats stats{};
  EXPECT_TRUE(DumpCpuStats(text, &stats));

  EXPECT_EQ(stats.entries, 1);
  EXPECT_EQ(stats.overrun, 2);
  EXPECT_EQ(stats.commit_overrun, 3);
  EXPECT_EQ(stats.bytes_read, 4);
  EXPECT_EQ(stats.oldest_event_ts, 5123.000);
  EXPECT_EQ(stats.now_ts, 6123.123);
  EXPECT_EQ(stats.dropped_events, 7);
  EXPECT_EQ(stats.read_events, 8);
}

}  // namespace
}  // namespace perfetto
