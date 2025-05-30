// ticker.c

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/timer.h>

#define CREATE_TRACE_POINTS
#include "trace/events/ticker.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Perfetto");
MODULE_DESCRIPTION("Ticker: A kernel module emitting example static tracepoint events.");
MODULE_VERSION("0.1");

static struct timer_list my_timer;
static unsigned int tick_count = 0;
static unsigned long timer_interval_ms = 1000;

static void my_timer_callback(struct timer_list *timer)
{
    // Fire the tracepoint, incrementing the tick count every time.
    // The function name is trace_<event_name> from the header file.
    trace_ticker_tick(tick_count++);

    // Re-arm the timer.
    mod_timer(&my_timer, jiffies + msecs_to_jiffies(timer_interval_ms));
}

static int __init ticker_init(void)
{
    pr_info("Ticker: Initializing...\n");

    timer_setup(&my_timer, my_timer_callback, 0);
    mod_timer(&my_timer, jiffies + msecs_to_jiffies(timer_interval_ms));

    pr_info("Ticker: Timer started.\n");
    pr_info("Ticker: View events under /sys/kernel/tracing/events/ticker/\n");

    return 0;
}

static void __exit ticker_exit(void)
{
    pr_info("Ticker: Exiting...\n");
    del_timer_sync(&my_timer);
    pr_info("Ticker: Timer stopped and module unloaded.\n");
}

module_init(ticker_init);
module_exit(ticker_exit);

