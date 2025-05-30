// trace/events/ticker.h

#undef TRACE_SYSTEM
#define TRACE_SYSTEM ticker

#if !defined(_TRACE_TICKER_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_TICKER_H

#include <linux/tracepoint.h>

TRACE_EVENT(ticker_tick,

    TP_PROTO(unsigned int count),

    TP_ARGS(count),

    TP_STRUCT__entry(
        __field(unsigned int, count)
    ),

    TP_fast_assign(
        __entry->count = count;
    ),

    TP_printk("count=%u",
        __entry->count
    )
);

#endif /* _TRACE_KEVIN_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
