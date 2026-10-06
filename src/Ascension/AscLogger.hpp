#pragma once
// The original's channel logger (AscLogger.cpp) for native subsystems.
#include <cstdint>
#include <string>

namespace AscLogger
{
    // FUN_10293640: the line and std::endl to Logs\<Channel>.txt (0 Trace .. 9 LargeAlloc); dropped when
    // the channel has no stream.
    void Write(uint32_t channel, const std::string& line);
    // FUN_10293aa0: Write, but each distinct line only once per channel (a seen-set keyed by the text).
    void WriteOnce(uint32_t channel, const std::string& line);
    // FUN_102936d0: Write of "[YYYY-MM-DD HH:MM:SS.mmm] <line>" (local time, milliseconds zero-padded),
    // used by the native managers' diagnostics (GMTicketMgr, PlayerTicketMgr, PlayerPollMgr).
    void WriteStamped(uint32_t channel, const std::string& line);
}
