// C_Logger (0x10291F50..0x102933A0): the original's channel logger. Logger (FUN_100a06d0, vftable
// Logger, static 0x10BDB1E8) keeps one ofstream per channel, opened truncated as
// <client>\Logs\<Channel>.txt (FUN_102917a0 / FUN_102912c0); FUN_10293640 writes a line to a channel
// that has a stream and drops it otherwise.
//
// Channels (0x10B45448): 0 Trace, 1 Info, 2 Debug, 3 Warning, 4 Error, 5 Fatal, 6 MissingFiles, 7 LUA,
// 8 LFG, 9 LargeAlloc. Each binding runs string.format (0x853C50) over its arguments, reads the result
// (luaL_checklstring(L, -1)), and when it is not empty writes
//   "HH:MM:SS [<short_src>:<currentline>] <text>"
// with the caller's frame from lua_getstack(L, 1) / lua_getinfo("nSl"). Info and Debug only log while
// the DLL's CVars logInfo / logDebug (default "1", FUN_101141e0) are non-zero. ClearLUA reopens LUA.txt.
//
// Reconstruction addition: every line is mirrored to Extensions.log as "[C_Logger:<Channel>] ...", and
// the LUA channel feeds AscRegistry's glue-load markers (GlueLogging.lua's "*GlueXML.toc Loading*").
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscAddons.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscLua.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CVar.hpp>
#include <cstdio>
#include <ctime>
#include <string>
#include <windows.h>

#include <Ascension/AscLogger.hpp>
#include <unordered_set>

namespace
{
    const char* const kChannels[10] = {"Trace", "Info", "Debug", "Warning", "Error", "Fatal", "MissingFiles",
        "LUA", "LFG", "LargeAlloc"};   // 0x10B45448
    FILE* g_streams[10] = {};

    // FUN_102917a0 / FUN_102912c0: (re)open <client>\Logs\<Channel>.txt, truncated.
    void Open(uint32_t channel)
    {
        if (g_streams[channel])
        {
            fclose(g_streams[channel]);
            g_streams[channel] = nullptr;
        }
        CreateDirectoryA("Logs", nullptr);
        const std::string path = std::string("Logs\\") + kChannels[channel] + ".txt";
        g_streams[channel] = fopen(path.c_str(), "w");
    }

    // FUN_10293640: the line and std::endl (a flush).
    void Write(uint32_t channel, const std::string& line)
    {
        if (channel >= 10 || !g_streams[channel])
            return;
        fputs(line.c_str(), g_streams[channel]);
        fputc('\n', g_streams[channel]);
        fflush(g_streams[channel]);
        AscLog::Printf("[C_Logger:%s] %s", kChannels[channel], line.c_str());
    }

    struct lua_Debug
    {
        int event;
        const char* name;
        const char* namewhat;
        const char* what;
        const char* source;
        int currentline;       // +0x14
        int nups, linedefined, lastlinedefined;
        char short_src[60];    // +0x24
        int i_ci;
    };
    typedef int(__cdecl* GetStack_t)(lua_State*, int, lua_Debug*);
    typedef int(__cdecl* GetInfo_t)(lua_State*, const char*, lua_Debug*);
    const GetStack_t lua_getstack = reinterpret_cast<GetStack_t>(0x84FE40);   // DAT_10bca4a4
    const GetInfo_t lua_getinfo = reinterpret_cast<GetInfo_t>(0x850A90);      // DAT_10bca4a8
    typedef int(__cdecl* LuaFn_t)(lua_State*);
    const LuaFn_t str_format = reinterpret_cast<LuaFn_t>(0x853C50);

    // The shared body of every level binding; `text` is the formatted message.
    void Log(lua_State* L, uint32_t channel, const std::string& text)
    {
        if (text.empty())
            return;
        lua_Debug ar = {};
        lua_getstack(L, 1, &ar);
        lua_getinfo(L, "nSl", &ar);
        char stamp[16];
        const time_t now = time(nullptr);
        tm local;
        localtime_s(&local, &now);
        strftime(stamp, sizeof(stamp), "%H:%M:%S", &local);
        Write(channel, std::string(stamp) + " [" + ar.short_src + ":" + std::to_string(ar.currentline) + "] " + text);
    }

    std::string Formatted(lua_State* L)
    {
        str_format(L);
        const char* s = AscScript::CheckString(L, -1);
        return s ? s : "";
    }

    // FUN_10114480 -> CVar::Lookup (0x767440); +0x30 the integer value.
    bool CVarOn(const char* name)
    {
        CVar* cv = CVar::Lookup(name);
        return cv && *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(cv) + 0x30) != 0;
    }

    int Trace(lua_State* L) { Log(L, 0, Formatted(L)); return 0; }     // FUN_10293100
    int Warning(lua_State* L) { Log(L, 3, Formatted(L)); return 0; }   // 0x102933A0
    int Error(lua_State* L) { Log(L, 4, Formatted(L)); return 0; }     // FUN_10292170
    int Fatal(lua_State* L) { Log(L, 5, Formatted(L)); return 0; }     // FUN_10292410
    int LFG(lua_State* L) { Log(L, 8, Formatted(L)); return 0; }       // FUN_102928b0

    int LUA(lua_State* L)   // FUN_10292b50
    {
        const std::string text = Formatted(L);
        Log(L, 7, text);
        return 0;
    }

    int Debug(lua_State* L)   // handler_Debug
    {
        if (CVarOn("logDebug"))
            Log(L, 2, Formatted(L));
        return 0;
    }

    int Info(lua_State* L)   // 0x102926B0
    {
        if (CVarOn("logInfo"))
            Log(L, 1, Formatted(L));
        return 0;
    }

    int ClearLUA(lua_State*)   // handler_ClearLUA: FUN_102912c0(7)
    {
        Open(7);
        return 0;
    }

    void Init()
    {
        for (uint32_t c = 0; c < 10; ++c)
            Open(c);
    }

    // ReportMetric(name, value) (FUN_10292df0): not while a known secure addon is running (0xD4139C,
    // SecureAddonMgr; its +0x10 override is never set). Otherwise, with both strings non-empty, CMSG 0x55A
    // {u32 length, name, u32 length, value} (no terminators) goes to the server and the binding returns 1
    // with nothing pushed -- the caller gets the top of the stack, as in the original. Else nothing.
    int ReportMetric(lua_State* L)
    {
        const char* running = *reinterpret_cast<const char* const*>(0xD4139C);
        if (running && AscAddons::IsSecure(running))
            return 0;
        const std::string name = AscScript::CheckString(L, 1);
        const std::string value = AscScript::CheckString(L, 2);
        if (name.empty() || value.empty())
            return 0;
        AscScript::Packet p(0x55A);
        p.U32(static_cast<uint32_t>(name.size())).Data(name.data(), static_cast<uint32_t>(name.size()));
        p.U32(static_cast<uint32_t>(value.size())).Data(value.data(), static_cast<uint32_t>(value.size()));
        p.Send();
        return 1;
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "ReportMetric", ReportMetric},   // a bare global: FUN_10293d30 registers it after C_Logger
        {"C_Logger", "Trace", Trace},
        {"C_Logger", "Error", Error},
        {"C_Logger", "Debug", Debug},
        {"C_Logger", "Info", Info},
        {"C_Logger", "Warning", Warning},
        {"C_Logger", "Fatal", Fatal},
        {"C_Logger", "LUA", LUA},
        {"C_Logger", "ClearLUA", ClearLUA},
        {"C_Logger", "LFG", LFG},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), Init);
}

void AscLogger::Write(uint32_t channel, const std::string& line) { ::Write(channel, line); }

// FUN_102936d0: _Xtime_get_ticks (100 ns since 1970) split into seconds (localtime64_s, put_time
// "%Y-%m-%d %H:%M:%S") and milliseconds (setfill('0') << setw(3)), then "[{}] {}" to the channel.
void AscLogger::WriteStamped(uint32_t channel, const std::string& line)
{
    if (channel >= 10 || !g_streams[channel])
        return;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const uint64_t ticks = ((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) - 116444736000000000ull;
    const __time64_t seconds = static_cast<__time64_t>(ticks / 10000000);
    const unsigned ms = static_cast<unsigned>((ticks / 10000) % 1000);
    tm local;
    _localtime64_s(&local, &seconds);
    char stamp[32];
    const size_t n = strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
    snprintf(stamp + n, sizeof(stamp) - n, ".%03u", ms);
    ::Write(channel, std::string("[") + stamp + "] " + line);
}

void AscLogger::WriteOnce(uint32_t channel, const std::string& line)
{
    static std::unordered_set<std::string> seen[10];
    if (channel >= 10 || !g_streams[channel])
        return;
    if (!seen[channel].insert(line).second)
        return;
    Write(channel, line);
}
