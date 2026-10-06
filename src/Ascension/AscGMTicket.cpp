// C_GMTicket (27) and C_PlayerTicket (14), transcribed from the original's GMTicketMgr and
// PlayerTicketMgr (E:\oniwow-artifacts\ghidra-ext\out\all).
//
// Every binding is gated on client 0x5191C0(0x11) (FUN_10111ce0) and returns nothing when it fails.
//
// Ticket record (0x110 bytes, reader FUN_10248730, Lua writer FUN_10248ea0), wire order:
//   str TicketID, str Title, u32 CreationDate, opt u32 LastMessageDate, str Creator, u8 Status,
//   opt str AISuggestion, u32 n x {str Command, str Reason}, u32 n x {str SQL, str Reason},
//   u32 n x {u32 BugTrackerReportID, str Reason}, opt str AISuggestionSummary,
//   u32 n x Message, u32 n x str AssignedTo, u8 IsLastMessageUnseen, u8 IsAnyMessageUnseen,
//   u8 IsLastMessageFromGM, u8 Priority, u8 Category, u8 IsClosedByCreator, opt u32 CloseDate,
//   str CreatorLocale, opt str OnlineCharacterName                  ("opt" = u8 present + value)
// Message (0x68, FUN_102434c0 / FUN_10243760): str TicketID, u32 MessageID, u8 IsFromGM,
//   str Sender, opt str Message, u8 GMOnly, u32 Timestamp,
//   u32 n x ReadReceipt {str TicketID, u32 MessageID, str ReadBy, u32 Timestamp} (FUN_10242a70).
//
// GMTicketMgr (FUN_1024d0c0, static 0x10BCB3C0): the ticket list, then GMTicketContainer
// (vtable 0x10B3E774) -- a filtered/sorted view over snapshot copies, driven by the generic
// FUN_102499c0 through virtuals: [2] filter predicate FUN_10251c00, [3] search FUN_10250940,
// [4] filter group FUN_10251e40, [7] default key FUN_10251e90, [9] sort key FUN_10252570.
//   SMSG 0x709 GM_TICKET_LIST (FUN_1024acc0): u8 clear, u8 reset, u32 n x ticket (appended).
//   SMSG 0x70A GM_TICKET_CREATED (FUN_1024aaa0): ticket; GM_TICKET_NEW_TICKET(id),
//        GM_TICKETS_UPDATED, view reset, GM_TICKET_FILTER_RESET.
//   SMSG 0x70B GM_TICKET_UPDATE (FUN_1024af30): ticket replacing the one with its id (unknown ids
//        are dropped); diff events, view reset, GM_TICKET_FILTER_RESET.
//   SMSG 0x71C REQUEST_PLAYER_INFO_RESULT (FUN_1024c790): str name, u8 found [player record, 0x198
//        bytes, ReadPlayerInfo]; appended to +0x88; REQUEST_PLAYER_INFO_RESULT(name) or (record name, name).
// PlayerTicketMgr (FUN_102552d0, static 0x10BE2560, +0x110 has-ticket):
//   SMSG 0x701 (handler_0x0701): the player's ticket; PLAYER_TICKET_UPDATE.
//   SMSG 0x702 (FUN_10253ec0): the player's ticket, diffed against the previous one.
//
// CMSG: 0x703 create, 0x705 close, 0x707 message, 0x70C status, 0x70E priority, 0x710 title,
// 0x712 approve, 0x714 decline, 0x717 assign, 0x719 reopen, 0x71D mark seen. An optional ticket id
// is written only when present (FUN_100d0c90).
#include <Ascension/AscAccount.hpp>
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscLogger.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace AscScript;

namespace
{
    struct ReadReceipt { std::string ticketId; uint32_t messageId = 0; std::string readBy; uint32_t timestamp = 0; };
    struct Message
    {
        std::string ticketId;
        uint32_t messageId = 0;
        bool isFromGM = false;
        std::string sender;
        bool hasMessage = false;
        std::string message;
        bool gmOnly = false;
        uint32_t timestamp = 0;
        std::vector<ReadReceipt> receipts;
    };
    struct Suggestion { std::string text, reason; };           // 0x30: Command|SQL, Reason
    struct BugReport { uint32_t id = 0; std::string reason; };  // 0x1C
    struct Ticket
    {
        std::string ticketId, title;
        uint32_t creationDate = 0;
        bool hasLastMessageDate = false;
        uint32_t lastMessageDate = 0;
        std::string creator;
        uint8_t status = 0;
        bool hasAISuggestion = false;
        std::string aiSuggestion;
        std::vector<Suggestion> commands, sql;
        std::vector<BugReport> bugReports;
        bool hasSummary = false;
        std::string summary;
        std::vector<Message> messages;
        std::vector<std::string> assignedTo;
        bool lastMessageUnseen = false, anyMessageUnseen = false, lastMessageFromGM = false;
        uint8_t priority = 0, category = 0;
        bool closedByCreator = false;
        bool hasCloseDate = false;
        uint32_t closeDate = 0;
        std::string creatorLocale;
        bool hasOnlineName = false;
        std::string onlineName;

        bool Closed() const { return status == 2 || status == 5; }   // CLOSED, LOCKED
    };

    // GM player record (0x198 bytes, reader FUN_102472d0, Lua writer FUN_10247d40).
    struct HistMessage   // 0x3C (FUN_102431a0 / FUN_10243340)
    {
        bool isFromGM = false, gmOnly = false;
        std::string sender;
        bool hasMessage = false;
        std::string message;
        uint32_t timestamp = 0;
    };
    struct HistTicket    // 0x48 (FUN_10242dc0 / FUN_10243010)
    {
        std::string ticketId, title;
        uint32_t creationDate = 0;
        bool hasCloseDate = false;
        uint32_t closeDate = 0;
        std::vector<HistMessage> messages;
    };
    struct Mute          // 0x54 (FUN_10242330 / FUN_102425c0)
    {
        uint32_t date = 0, time = 0;
        std::string reason, by;
        uint8_t mode = 0;
        std::string realm;
    };
    struct Ban           // 0x40 (FUN_10241e20 / FUN_10242020)
    {
        uint32_t date = 0, remaining = 0;
        bool active = false;
        uint32_t unbanDate = 0;
        std::string reason, by;
    };
    struct Warning { std::string realm, character, reason; uint32_t timestamp = 0; };   // 0x4C (FUN_10242720 / FUN_10242970)
    struct Note { uint32_t id = 0; std::string gm, message; uint32_t date = 0; };       // 0x38 (FUN_10242170 / FUN_1023fce0)
    struct PlayerInfo
    {
        std::string name;                                   // +0x00
        uint32_t guid = 0;                                  // +0x18
        uint8_t level = 0, race = 0, classId = 0;           // +0x1C..+0x1E
        uint32_t gameModeMask = 0;                          // +0x20
        bool hasItemLevel = false; float itemLevel = 0;     // +0x24
        bool hasMoney = false; uint32_t money = 0;          // +0x2C
        std::vector<std::pair<uint32_t, uint32_t>> challenges;   // +0x34 {ChallengeID, ChallengeLevel}
        bool hasMap = false; uint32_t map = 0;              // +0x40
        bool hasX = false, hasY = false, hasZ = false;      // +0x48 / +0x50 / +0x58
        float x = 0, y = 0, z = 0;
        bool hasZone = false; uint32_t zone = 0;            // +0x60
        bool hasArea = false; uint32_t area = 0;            // +0x68
        uint32_t guildId = 0;                               // +0x70
        bool hasGuildName = false; std::string guildName;   // +0x74
        bool hasRank = false; uint8_t rank = 0;             // +0x90
        bool hasRankName = false; std::string rankName;     // +0x94
        uint32_t accountId = 0;                             // +0xB0
        std::string accountName;                            // +0xB4
        uint32_t gmLevel = 0;                               // +0xCC
        bool hasDiscord = false; std::string discord;       // +0xD0
        bool hasHwid = false; std::string hwid;             // +0xEC
        bool hasHwid2 = false; std::string hwid2;           // +0x108
        bool hasOnlineName = false; std::string onlineName; // +0x124
        std::vector<HistTicket> tickets; uint32_t numTickets = 0;    // +0x140 / +0x14C
        std::vector<Mute> mutes; uint32_t numMutes = 0;              // +0x150 / +0x15C
        std::vector<Ban> bans; uint32_t numBans = 0;                 // +0x160 / +0x16C
        std::vector<Warning> warnings; uint32_t numWarnings = 0;     // +0x170 / +0x17C
        std::vector<Note> notes; uint32_t numNotes = 0;              // +0x180 / +0x18C
        bool hasLatency = false; uint32_t latency = 0;               // +0x190
    };

    // Enum name tables (pointer, length pairs in the original).
    const char* const kStatus[7] = {"NONE", "OPEN", "CLOSED", "WAITING_FOR_PLAYER",
        "WAITING_FOR_GAME_MASTER", "LOCKED", "ESCALATED_SECURITY_LEVEL"};                      // 0x10B3E28C
    const char* const kPriority[4] = {"LOW", "MEDIUM", "HIGH", "URGENT"};                     // 0x10B3E6C0
    const char* const kCategory[10] = {"GM_TICKET_CATEGORY_OTHER", "GM_TICKET_CATEGORY_TALENTS",
        "GM_TICKET_CATEGORY_DUNGEONS", "GM_TICKET_CATEGORY_ITEMS",
        "GM_TICKET_CATEGORY_SPELLS_AND_ABILITIES", "GM_TICKET_CATEGORY_WEBSITE_OR_LAUNCHER",
        "GM_TICKET_CATEGORY_MYSTIC_ENCHANTS", "GM_TICKET_CATEGORY_RAIDS", "GM_TICKET_CATEGORY_QUESTS",
        "GM_TICKET_CATEGORY_UI_OR_ADDONS"};                                                    // 0x10B3E4F0
    const char* const kFilters[8] = {"FILTER_NONE", "FILTER_OPEN", "FILTER_CLOSED", "FILTER_ONLINE",
        "FILTER_OFFLINE", "FILTER_ASSIGNED_TO_ME", "FILTER_ASSIGNED_TO_OTHER",
        "FILTER_ASSIGNED_TO_NONE"};                                                            // 0x10B3E400
    const char* const kSorts[5] = {"SORT_NONE", "SORT_CREATION_TIME_ASCENDING",
        "SORT_CREATION_TIME_DESCENDING", "SORT_LAST_PLAYER_REPLY_ASCENDING",
        "SORT_LAST_PLAYER_REPLY_DESCENDING"};                                                  // 0x10B3E588
    const char* const kSecurity[8] = {"SEC_PLAYER", "SEC_MODERATOR", "SEC_GAMEMASTER", "SEC_GAMEMASTER2",
        "SEC_GAMEMASTER3", "SEC_GAMEMASTER4", "SEC_ADMINISTRATOR", "SEC_CONSOLE"};              // 0x10B3E540

    // ---- state ----------------------------------------------------------------------------------
    std::vector<Ticket> g_tickets;          // GMTicketMgr +0x00
    std::vector<Ticket> g_base;             // container base: snapshot copies, default-key order
    std::vector<size_t> g_view;             // container view: indices into g_base
    bool g_viewDirty = true;                // container +0x78
    std::vector<PlayerInfo> g_playerInfo;   // GMTicketMgr +0x88 (appended per result, never deduplicated)
    Ticket g_current;                       // PlayerTicketMgr
    bool g_hasCurrent = false;              // PlayerTicketMgr +0x110

    bool Gate() { return reinterpret_cast<int(__cdecl*)(uint32_t)>(0x5191C0)(0x11) != 0; }   // FUN_10111ce0

    // The active player's name: object vtable +0xD8.
    const char* PlayerName(uint8_t* player)
    {
        typedef const char*(__thiscall* NameFn)(void*);
        return reinterpret_cast<NameFn>((*reinterpret_cast<void***>(player))[0xD8 / 4])(player);
    }

    // ---- wire -----------------------------------------------------------------------------------
    template <class T> T Read(CDataStore* p)
    {
        T v;
        memcpy(&v, p->m_buffer + p->m_read, sizeof(T));
        p->m_read += sizeof(T);
        return v;
    }
    std::string ReadStr(CDataStore* p)
    {
        std::string s(reinterpret_cast<const char*>(p->m_buffer + p->m_read));
        p->m_read += static_cast<int32_t>(s.size() + 1);
        return s;
    }
    bool ReadOptStr(CDataStore* p, std::string& out)   // FUN_1023a8e0
    {
        if (!Read<uint8_t>(p))
            return false;
        out = ReadStr(p);
        return true;
    }
    bool ReadOptU32(CDataStore* p, uint32_t& out)
    {
        if (!Read<uint8_t>(p))
            return false;
        out = Read<uint32_t>(p);
        return true;
    }

    Message ReadMessage(CDataStore* p)   // FUN_102434c0
    {
        Message m;
        m.ticketId = ReadStr(p);
        m.messageId = Read<uint32_t>(p);
        m.isFromGM = Read<uint8_t>(p) != 0;
        m.sender = ReadStr(p);
        m.hasMessage = ReadOptStr(p, m.message);
        m.gmOnly = Read<uint8_t>(p) != 0;
        m.timestamp = Read<uint32_t>(p);
        for (uint32_t n = Read<uint32_t>(p); n; --n)   // FUN_1023b9d0 / FUN_10242a70
        {
            ReadReceipt r;
            r.ticketId = ReadStr(p);
            r.messageId = Read<uint32_t>(p);
            r.readBy = ReadStr(p);
            r.timestamp = Read<uint32_t>(p);
            m.receipts.push_back(r);
        }
        return m;
    }

    std::vector<Suggestion> ReadSuggestions(CDataStore* p)   // FUN_1023c270 / FUN_10242c30
    {
        std::vector<Suggestion> v;
        for (uint32_t n = Read<uint32_t>(p); n; --n)
        {
            Suggestion s;
            s.text = ReadStr(p);
            s.reason = ReadStr(p);
            v.push_back(s);
        }
        return v;
    }

    Ticket ReadTicket(CDataStore* p)   // FUN_1023a650 -> FUN_10248730
    {
        Ticket t;
        t.ticketId = ReadStr(p);
        t.title = ReadStr(p);
        t.creationDate = Read<uint32_t>(p);
        t.hasLastMessageDate = ReadOptU32(p, t.lastMessageDate);
        t.creator = ReadStr(p);
        t.status = Read<uint8_t>(p);
        t.hasAISuggestion = ReadOptStr(p, t.aiSuggestion);
        t.commands = ReadSuggestions(p);
        t.sql = ReadSuggestions(p);
        for (uint32_t n = Read<uint32_t>(p); n; --n)   // FUN_1023bdf0
        {
            BugReport b;
            b.id = Read<uint32_t>(p);
            b.reason = ReadStr(p);
            t.bugReports.push_back(b);
        }
        t.hasSummary = ReadOptStr(p, t.summary);
        for (uint32_t n = Read<uint32_t>(p); n; --n)   // FUN_1023cfc0
            t.messages.push_back(ReadMessage(p));
        for (uint32_t n = Read<uint32_t>(p); n; --n)   // FUN_1023d4d0
            t.assignedTo.push_back(ReadStr(p));
        t.lastMessageUnseen = Read<uint8_t>(p) != 0;
        t.anyMessageUnseen = Read<uint8_t>(p) != 0;
        t.lastMessageFromGM = Read<uint8_t>(p) != 0;
        t.priority = Read<uint8_t>(p);
        t.category = Read<uint8_t>(p);
        t.closedByCreator = Read<uint8_t>(p) != 0;
        t.hasCloseDate = ReadOptU32(p, t.closeDate);
        t.creatorLocale = ReadStr(p);
        t.hasOnlineName = ReadOptStr(p, t.onlineName);
        return t;
    }

    Ticket* FindTicket(const std::string& id)   // FUN_1024a630 over the manager's list
    {
        for (Ticket& t : g_tickets)
            if (t.ticketId == id)
                return &t;
        return nullptr;
    }

    template <class T> bool ReadOpt(CDataStore* p, T& out)   // u8 present + value
    {
        if (!Read<uint8_t>(p))
            return false;
        out = Read<T>(p);
        return true;
    }

    PlayerInfo ReadPlayerInfo(CDataStore* p)   // FUN_1023a4a0 -> FUN_102472d0
    {
        PlayerInfo r;
        r.name = ReadStr(p);
        r.guid = Read<uint32_t>(p);
        r.level = Read<uint8_t>(p);
        r.race = Read<uint8_t>(p);
        r.classId = Read<uint8_t>(p);
        r.gameModeMask = Read<uint32_t>(p);
        r.hasItemLevel = ReadOpt(p, r.itemLevel);
        r.hasMoney = ReadOpt(p, r.money);
        for (uint32_t n = Read<uint32_t>(p); n; --n)   // FUN_10117120
        {
            const uint32_t id = Read<uint32_t>(p);
            r.challenges.emplace_back(id, Read<uint32_t>(p));
        }
        r.hasMap = ReadOpt(p, r.map);
        r.hasX = ReadOpt(p, r.x);
        r.hasY = ReadOpt(p, r.y);
        r.hasZ = ReadOpt(p, r.z);
        r.hasZone = ReadOpt(p, r.zone);
        r.hasArea = ReadOpt(p, r.area);
        r.guildId = Read<uint32_t>(p);
        r.hasGuildName = ReadOptStr(p, r.guildName);
        r.hasRank = ReadOpt(p, r.rank);
        r.hasRankName = ReadOptStr(p, r.rankName);
        r.accountId = Read<uint32_t>(p);
        r.accountName = ReadStr(p);
        r.gmLevel = Read<uint32_t>(p);
        r.hasDiscord = ReadOptStr(p, r.discord);
        r.hasHwid = ReadOptStr(p, r.hwid);
        r.hasHwid2 = ReadOptStr(p, r.hwid2);
        r.hasOnlineName = ReadOptStr(p, r.onlineName);
        for (uint32_t n = Read<uint32_t>(p); n; --n)   // FUN_1023c6c0
        {
            HistTicket t;
            t.ticketId = ReadStr(p);
            t.title = ReadStr(p);
            t.creationDate = Read<uint32_t>(p);
            t.hasCloseDate = ReadOptU32(p, t.closeDate);
            for (uint32_t m = Read<uint32_t>(p); m; --m)   // FUN_1023cc40
            {
                HistMessage msg;
                msg.isFromGM = Read<uint8_t>(p) != 0;
                msg.gmOnly = Read<uint8_t>(p) != 0;
                msg.sender = ReadStr(p);
                msg.hasMessage = ReadOptStr(p, msg.message);
                msg.timestamp = Read<uint32_t>(p);
                t.messages.push_back(msg);
            }
            r.tickets.push_back(t);
        }
        r.numTickets = Read<uint32_t>(p);
        for (uint32_t n = Read<uint32_t>(p); n; --n)   // FUN_1023b1a0
        {
            Mute m;
            m.date = Read<uint32_t>(p);
            m.time = Read<uint32_t>(p);
            m.reason = ReadStr(p);
            m.by = ReadStr(p);
            m.mode = Read<uint8_t>(p);
            m.realm = ReadStr(p);
            r.mutes.push_back(m);
        }
        r.numMutes = Read<uint32_t>(p);
        for (uint32_t n = Read<uint32_t>(p); n; --n)   // FUN_1023aaa0
        {
            Ban b;
            b.date = Read<uint32_t>(p);
            b.remaining = Read<uint32_t>(p);
            b.active = Read<uint8_t>(p) != 0;
            b.unbanDate = Read<uint32_t>(p);
            b.reason = ReadStr(p);
            b.by = ReadStr(p);
            r.bans.push_back(b);
        }
        r.numBans = Read<uint32_t>(p);
        for (uint32_t n = Read<uint32_t>(p); n; --n)   // FUN_1023b5c0
        {
            Warning w;
            w.realm = ReadStr(p);
            w.character = ReadStr(p);
            w.reason = ReadStr(p);
            w.timestamp = Read<uint32_t>(p);
            r.warnings.push_back(w);
        }
        r.numWarnings = Read<uint32_t>(p);
        for (uint32_t n = Read<uint32_t>(p); n; --n)   // FUN_1023ae00
        {
            Note note;
            note.id = Read<uint32_t>(p);
            note.gm = ReadStr(p);
            note.message = ReadStr(p);
            note.date = Read<uint32_t>(p);
            r.notes.push_back(note);
        }
        r.numNotes = Read<uint32_t>(p);
        r.hasLatency = ReadOptU32(p, r.latency);   // FUN_1023a890
        return r;
    }

    // ---- Lua writers ----------------------------------------------------------------------------
    void SetStr(lua_State* L, const char* k, const std::string& v)
    {
        AscLua::lua_pushstring(L, k);
        PushStr(L, v.c_str());
        AscLua::lua_settable(L, -3);
    }
    void SetOptStr(lua_State* L, const char* k, bool has, const std::string& v)   // FUN_10240c10
    {
        AscLua::lua_pushstring(L, k);
        if (has) PushStr(L, v.c_str()); else AscLua::lua_pushnil(L);
        AscLua::lua_settable(L, -3);
    }
    void SetU32(lua_State* L, const char* k, uint32_t v)   // FUN_1009b2a0 = lua_pushinteger
    {
        AscLua::lua_pushstring(L, k);
        AscLua::lua_pushinteger(L, static_cast<int>(v));
        AscLua::lua_settable(L, -3);
    }
    void SetOptU32(lua_State* L, const char* k, bool has, uint32_t v)
    {
        AscLua::lua_pushstring(L, k);
        if (has) AscLua::lua_pushinteger(L, static_cast<int>(v)); else AscLua::lua_pushnil(L);
        AscLua::lua_settable(L, -3);
    }
    void SetBool(lua_State* L, const char* k, bool v)
    {
        AscLua::lua_pushstring(L, k);
        PushBool(L, v);
        AscLua::lua_settable(L, -3);
    }
    // FUN_10241840 / FUN_102416e0 / FUN_10241580: the enum's name, or UNEXPECTED_ENUM_VALUE_<n>.
    void SetEnum(lua_State* L, const char* k, uint8_t v, const char* const* names, uint32_t count)
    {
        AscLua::lua_pushstring(L, k);
        if (v < count)
            AscLua::lua_pushstring(L, names[v]);
        else
            AscLua::lua_pushstring(L, ("UNEXPECTED_ENUM_VALUE_" + std::to_string(v)).c_str());
        AscLua::lua_settable(L, -3);
    }
    // Array tables are keyed with lua_pushnumber(i), as the original's vector writers do.
    void BeginEntry(lua_State* L, uint32_t i) { AscLua::lua_pushnumber(L, static_cast<double>(i)); }

    void PushMessage(lua_State* L, const Message& m)   // FUN_10243760
    {
        AscLua::lua_createtable(L, 0, 8);
        AscLua::lua_checkstack(L, 2);
        SetStr(L, "TicketID", m.ticketId);
        SetU32(L, "MessageID", m.messageId);
        SetBool(L, "IsFromGM", m.isFromGM);
        SetStr(L, "Sender", m.sender);
        SetOptStr(L, "Message", m.hasMessage, m.message);
        SetBool(L, "GMOnly", m.gmOnly);
        SetU32(L, "Timestamp", m.timestamp);
        AscLua::lua_pushstring(L, "ReadReceipts");
        AscLua::lua_createtable(L, static_cast<int>(m.receipts.size()), 0);   // FUN_1023fee0
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < m.receipts.size(); ++i)
        {
            const ReadReceipt& r = m.receipts[i];
            BeginEntry(L, i + 1);
            AscLua::lua_createtable(L, 0, 4);
            AscLua::lua_checkstack(L, 2);
            SetStr(L, "TicketID", r.ticketId);
            SetU32(L, "MessageID", r.messageId);
            SetStr(L, "ReadBy", r.readBy);
            SetU32(L, "Timestamp", r.timestamp);
            AscLua::lua_settable(L, -3);
        }
        AscLua::lua_settable(L, -3);
    }

    void PushMessages(lua_State* L, const std::vector<Message>& v)   // FUN_10240750
    {
        AscLua::lua_createtable(L, static_cast<int>(v.size()), 0);
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < v.size(); ++i)
        {
            BeginEntry(L, i + 1);
            PushMessage(L, v[i]);
            AscLua::lua_settable(L, -3);
        }
    }

    void SetSuggestions(lua_State* L, const char* k, const char* textKey, const std::vector<Suggestion>& v)
    {                                                                     // FUN_10240260 / FUN_10240400
        AscLua::lua_pushstring(L, k);
        AscLua::lua_createtable(L, static_cast<int>(v.size()), 0);
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < v.size(); ++i)
        {
            BeginEntry(L, i + 1);
            AscLua::lua_createtable(L, 0, 2);
            AscLua::lua_checkstack(L, 2);
            SetStr(L, textKey, v[i].text);
            SetStr(L, "Reason", v[i].reason);
            AscLua::lua_settable(L, -3);
        }
        AscLua::lua_settable(L, -3);
    }

    void PushTicket(lua_State* L, const Ticket& t)   // FUN_10240910 -> FUN_10248ea0
    {
        AscLua::lua_createtable(L, 0, 0x16);
        AscLua::lua_checkstack(L, 2);
        SetStr(L, "TicketID", t.ticketId);
        SetStr(L, "Title", t.title);
        SetU32(L, "CreationDate", t.creationDate);
        SetOptU32(L, "LastMessageDate", t.hasLastMessageDate, t.lastMessageDate);
        SetStr(L, "Creator", t.creator);
        SetEnum(L, "Status", t.status, kStatus, 7);
        SetOptStr(L, "AISuggestion", t.hasAISuggestion, t.aiSuggestion);
        SetSuggestions(L, "AISuggestedCommands", "Command", t.commands);
        SetSuggestions(L, "AISuggestedSQL", "SQL", t.sql);
        AscLua::lua_pushstring(L, "AISuggestedBugTrackerReports");   // FUN_102400d0
        AscLua::lua_createtable(L, static_cast<int>(t.bugReports.size()), 0);
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < t.bugReports.size(); ++i)
        {
            BeginEntry(L, i + 1);
            AscLua::lua_createtable(L, 0, 2);
            AscLua::lua_checkstack(L, 2);
            SetU32(L, "BugTrackerReportID", t.bugReports[i].id);
            SetStr(L, "Reason", t.bugReports[i].reason);
            AscLua::lua_settable(L, -3);
        }
        AscLua::lua_settable(L, -3);
        SetOptStr(L, "AISuggestionSummary", t.hasSummary, t.summary);
        AscLua::lua_pushstring(L, "Messages");
        PushMessages(L, t.messages);
        AscLua::lua_settable(L, -3);
        AscLua::lua_pushstring(L, "AssignedTo");   // FUN_100beb90
        AscLua::lua_createtable(L, static_cast<int>(t.assignedTo.size()), 0);
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < t.assignedTo.size(); ++i)
        {
            BeginEntry(L, i + 1);
            PushStr(L, t.assignedTo[i].c_str());
            AscLua::lua_settable(L, -3);
        }
        AscLua::lua_settable(L, -3);
        SetBool(L, "IsLastMessageUnseen", t.lastMessageUnseen);
        SetBool(L, "IsAnyMessageUnseen", t.anyMessageUnseen);
        SetBool(L, "IsLastMessageFromGM", t.lastMessageFromGM);
        SetEnum(L, "Priority", t.priority, kPriority, 4);
        SetEnum(L, "Category", t.category, kCategory, 10);
        SetBool(L, "IsClosedByCreator", t.closedByCreator);
        SetOptU32(L, "CloseDate", t.hasCloseDate, t.closeDate);
        SetStr(L, "CreatorLocale", t.creatorLocale);
        SetOptStr(L, "OnlineCharacterName", t.hasOnlineName, t.onlineName);
    }

    // ---- GMTicketContainer ----------------------------------------------------------------------
    void ResetView()   // FUN_1024d1f0
    {
        g_base.clear();
        g_view.clear();
        g_viewDirty = true;
    }

    // [7] FUN_10251e90: open before closed, a close date before none, then newest first (CloseDate
    // when closed and set, else CreationDate).
    bool DefaultLess(const Ticket& a, const Ticket& b)
    {
        auto key = [](const Ticket& t, int& c0, int& c1, uint32_t& inv)
        {
            c0 = t.Closed() ? 1 : 0;
            c1 = t.hasCloseDate ? 0 : 1;
            inv = ~((t.Closed() && t.hasCloseDate) ? t.closeDate : t.creationDate);
        };
        int a0, a1, b0, b1;
        uint32_t ai, bi;
        key(a, a0, a1, ai);
        key(b, b0, b1, bi);
        if (a0 != b0) return a0 < b0;
        if (a1 != b1) return a1 < b1;
        return ai < bi;
    }

    // [9] FUN_10252570, compared as unsigned 64-bit.
    uint64_t SortKey(uint32_t mode, const Ticket& t)
    {
        switch (mode)
        {
        case 1: return t.creationDate;
        case 2: return (0xFFFFFFFFull << 32) | static_cast<uint32_t>(~t.creationDate);
        case 3:
        case 4:
        {
            uint64_t k = ~0ull;   // no reply from the player: last either way
            for (auto it = t.messages.rbegin(); it != t.messages.rend(); ++it)
                if (!it->isFromGM)
                {
                    k = it->timestamp;
                    break;
                }
            if (mode == 4 && k != ~0ull)
                k = ~k;
            return k;
        }
        default: return 0;
        }
    }

    // [4] FUN_10251e40.
    uint32_t FilterGroup(uint32_t f)
    {
        if (f == 1 || f == 2) return 1;
        if (f == 3 || f == 4) return 2;
        if (f >= 5 && f <= 7) return 3;
        return 0;
    }

    // [2] FUN_10251c00.
    bool FilterPasses(uint32_t f, const Ticket& t)
    {
        switch (f)
        {
        case 1: return !t.Closed();
        case 2: return t.Closed();
        case 3: return t.hasOnlineName;
        case 4: return !t.hasOnlineName;
        case 5:
        case 6:
        {
            uint8_t* player = ActivePlayer();
            if (!player || t.assignedTo.empty())
                return false;
            const std::string me = PlayerName(player);
            const bool mine = std::find(t.assignedTo.begin(), t.assignedTo.end(), me) != t.assignedTo.end();
            return f == 5 ? mine : !mine;
        }
        case 7: return t.assignedTo.empty();
        default: return false;
        }
    }

    std::string Lower(std::string s)
    {
        for (char& c : s)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return s;
    }
    bool Contains(const std::string& field, const std::string& lowerSearch)
    {
        return Lower(field).find(lowerSearch) != std::string::npos;
    }

    // [3] FUN_10250940: a case-insensitive substring of any text the ticket carries.
    bool SearchMatches(const std::string& s, const Ticket& t)
    {
        if (s.empty())
            return true;
        if (Contains(t.title, s) || Contains(t.creator, s) || Contains(t.ticketId, s))
            return true;
        for (const std::string& a : t.assignedTo)
            if (Contains(a, s))
                return true;
        for (const Message& m : t.messages)
            if ((m.hasMessage && Contains(m.message, s)) || Contains(m.sender, s))
                return true;
        if (t.hasAISuggestion && Contains(t.aiSuggestion, s))
            return true;
        if (t.hasSummary && Contains(t.summary, s))
            return true;
        for (const Suggestion& c : t.commands)
            if (Contains(c.text, s) || Contains(c.reason, s))
                return true;
        for (const Suggestion& c : t.sql)
            if (Contains(c.text, s) || Contains(c.reason, s))
                return true;
        for (const BugReport& b : t.bugReports)
            if (Contains(b.reason, s))
                return true;
        return false;
    }

    // FUN_100c2840: "$<c><digits>" tokens. The container's pair test ([1] FUN_100cc230) is
    // `xor al, al`, so for GM tickets any such token rejects every ticket.
    bool HasSearchToken(const std::string& s)
    {
        size_t pos = 0;
        while ((pos = s.find('$', pos)) != std::string::npos)
        {
            size_t end = s.find(' ', pos);
            if (end == std::string::npos)
                end = s.size();
            if (pos + 2 < end)
            {
                bool digits = true;
                for (size_t i = pos + 2; i < end; ++i)
                    if (!isdigit(static_cast<unsigned char>(s[i])))
                        digits = false;
                if (digits)
                    return true;
            }
            pos = end;
        }
        return false;
    }

    // FUN_102499c0(search, filters, sorts, 1).
    void ApplyFilter(const std::string& search, const std::vector<uint32_t>& filters,
                     const std::vector<uint32_t>& sorts)
    {
        if (g_base.empty())   // FUN_100c5910: rebuild from the manager, default-key order
        {
            g_base = g_tickets;
            std::stable_sort(g_base.begin(), g_base.end(), DefaultLess);
            g_view.clear();
            for (size_t i = 0; i < g_base.size(); ++i)
                g_view.push_back(i);
            g_viewDirty = true;
            if (g_base.empty())
            {
                g_viewDirty = false;
                return;
            }
        }
        const bool token = HasSearchToken(search);
        const std::string lowered = Lower(search);

        g_view.clear();
        for (size_t i = 0; i < g_base.size(); ++i)
        {
            const Ticket& t = g_base[i];
            bool ok = !token;
            // Groups AND together; the filters inside a group OR. Group 0 (unknown) must pass alone.
            for (uint32_t group = 0; ok && group <= 3; ++group)
            {
                bool any = false, seen = false;
                for (uint32_t f : filters)
                {
                    if (FilterGroup(f) != group)
                        continue;
                    seen = true;
                    if (FilterPasses(f, t))
                        any = true;
                    else if (group == 0)
                        ok = false;
                }
                if (seen && group != 0 && !any)
                    ok = false;
            }
            if (ok && SearchMatches(lowered, t))
                g_view.push_back(i);
        }
        if (!sorts.empty())
            std::stable_sort(g_view.begin(), g_view.end(), [&](size_t a, size_t b)
            {
                for (uint32_t mode : sorts)
                {
                    const uint64_t ka = SortKey(mode, g_base[a]), kb = SortKey(mode, g_base[b]);
                    if (ka != kb)
                        return ka < kb;
                }
                return false;
            });
        g_viewDirty = false;
    }

    // ---- argument readers -----------------------------------------------------------------------
    bool Present(lua_State* L, int idx) { return AscLua::lua_type(L, idx) > 0; }   // DAT_10bc93c4 = lua_type

    // FUN_100d0970: the string at idx when present (type > 0).
    bool OptStringAt(lua_State* L, int idx, std::string& out)
    {
        if (!Present(L, idx))
            return false;
        out = CheckString(L, idx);
        return true;
    }

    // lua_tovalue<enum>: exact name match, else logs and reads as 0 (FUN_102419a0 / FUN_10241be0 /
    // FUN_10253070).
    uint8_t EnumArg(lua_State* L, int idx, const char* const* names, uint32_t count, const char* enumName)
    {
        const char* s = CheckString(L, idx);
        for (uint32_t i = 0; i < count; ++i)
            if (strcmp(s, names[i]) == 0)
                return static_cast<uint8_t>(i);
        AscLog::Printf("lua_tovalue<enum %s>: Unknown enum string '%s'", enumName, s);
        return 0;
    }

    // FUN_10239400 / FUN_10252c30: optional ticket id at 1.
    bool ReadOptId(lua_State* L, bool& has, std::string& id)
    {
        if (!ValidateInput(L, {Present(L, 1) ? STRING : NIL}))
            return false;
        has = OptStringAt(L, 1, id);
        return true;
    }

    // FUN_100d0770 (via FUN_10239670): two strings.
    bool ReadTwoStrings(lua_State* L, std::string& a, std::string& b)
    {
        if (!ValidateInput(L, {STRING, STRING}))
            return false;
        a = CheckString(L, 1);
        b = CheckString(L, 2);
        return true;
    }

    // FUN_100e96d0: string, number.
    bool ReadStringNumber(lua_State* L, std::string& s, uint32_t& n)
    {
        if (!ValidateInput(L, {STRING, NUMBER}))
            return false;
        s = CheckString(L, 1);
        n = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        return true;
    }

    // FUN_10239810: optional id, number.
    bool ReadMarkSeen(lua_State* L, bool& has, std::string& id, uint32_t& messageId)
    {
        if (!ValidateInput(L, {Present(L, 1) ? STRING : NIL, NUMBER}))
            return false;
        has = OptStringAt(L, 1, id);
        messageId = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        return true;
    }

    // FUN_1023a2e0: optional id, message, GM-only flag.
    bool ReadSendMessage(lua_State* L, bool& has, std::string& id, std::string& text, bool& gmOnly)
    {
        if (!ValidateInput(L, {Present(L, 1) ? STRING : NIL, STRING, BOOLEAN}))
            return false;
        has = OptStringAt(L, 1, id);
        text = CheckString(L, 2);
        gmOnly = AscLua::lua_toboolean(L, 3) != 0;
        return true;
    }

    // FUN_10239c70 / FUN_10239e10: id, enum name.
    bool ReadIdEnum(lua_State* L, std::string& id, uint8_t& v, const char* const* names, uint32_t count,
                    const char* enumName)
    {
        if (!ValidateInput(L, {STRING, STRING}))
            return false;
        id = CheckString(L, 1);
        v = EnumArg(L, 2, names, count, enumName);
        return true;
    }

    // FUN_10252d80: priority, category, then three strings.
    bool ReadCreate(lua_State* L, uint8_t& priority, uint8_t& category, std::string& a, std::string& b,
                    std::string& c)
    {
        if (!ValidateInput(L, {STRING, STRING, STRING, STRING, STRING}))
            return false;
        priority = EnumArg(L, 1, kPriority, 4, "GMTicketPriority");
        category = EnumArg(L, 2, kCategory, 10, "GMTicketCategory");
        a = CheckString(L, 3);
        b = CheckString(L, 4);
        c = CheckString(L, 5);
        return true;
    }

    int PushTrueNil(lua_State* L)
    {
        PushBool(L, true);
        AscLua::lua_pushnil(L);
        return 2;
    }

    void PutOptId(Packet& p, bool has, const std::string& id)   // FUN_100d0c90
    {
        if (has)
            p.Str(id.c_str());
    }

    // ---- C_GMTicket -----------------------------------------------------------------------------
    int GetNumTickets(lua_State* L)
    {
        if (!Gate()) return 0;
        PushNum(L, static_cast<double>(g_view.size()));
        return 1;
    }

    int GetTicketAtIndex(lua_State* L)
    {
        if (!Gate() || !ValidateInput(L, {NUMBER}))
            return 0;
        const int32_t index = ToInt(CheckNumber(L, 1));
        if (index == 0)
            return 0;
        const uint32_t i = static_cast<uint32_t>(index) - 1;
        if (i < g_view.size())
            PushTicket(L, g_base[g_view[i]]);
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    int GetTicketByID(lua_State* L)
    {
        std::string id;
        if (!Gate() || !ReadString(L, id))
            return 0;
        if (const Ticket* t = FindTicket(id)) PushTicket(L, *t); else AscLua::lua_pushnil(L);
        return 1;
    }

    int GetTicketMessage(lua_State* L)
    {
        std::string id;
        uint32_t messageId;
        if (!Gate() || !ReadStringNumber(L, id, messageId))
            return 0;
        if (const Ticket* t = FindTicket(id))
            for (const Message& m : t->messages)
                if (m.messageId == messageId)
                {
                    PushMessage(L, m);
                    return 1;
                }
        AscLua::lua_pushnil(L);
        return 1;
    }

    int GetTicketMessages(lua_State* L)
    {
        std::string id;
        if (!Gate() || !ReadString(L, id))
            return 0;
        if (const Ticket* t = FindTicket(id)) PushMessages(L, t->messages); else AscLua::lua_pushnil(L);
        return 1;
    }

    int GetAutomatedMessage(lua_State* L)
    {
        std::string id;
        if (!Gate() || !ReadString(L, id))
            return 0;
        const Ticket* t = FindTicket(id);
        if (t && t->hasAISuggestion) PushStr(L, t->aiSuggestion.c_str()); else AscLua::lua_pushnil(L);
        return 1;
    }

    int GetNumSuggestions(lua_State* L)
    {
        std::string id;
        if (!Gate() || !ReadString(L, id))
            return 0;
        const Ticket* t = FindTicket(id);
        if (!t)
            return 0;
        PushNum(L, static_cast<double>(t->sql.size() + t->commands.size() + t->bugReports.size()
                                       + (t->hasSummary ? 1 : 0)));
        return 1;
    }

    // FUN_10240ac0 with the table flag set: {Type, Command, Reason}.
    void PushSuggestion(lua_State* L, const char* type, const std::string& command, const std::string& reason)
    {
        AscLua::lua_createtable(L, 0, 3);
        AscLua::lua_checkstack(L, 2);
        SetStr(L, "Type", type);
        SetStr(L, "Command", command);
        SetStr(L, "Reason", reason);
    }

    // Index 1 is always the summary; then commands, SQL, bug tracker reports.
    int GetSuggestionAtIndex(lua_State* L)
    {
        std::string id;
        uint32_t index;
        if (!Gate() || !ReadStringNumber(L, id, index) || index == 0)
            return 0;
        const Ticket* t = FindTicket(id);
        if (!t)
            return 0;
        if (index == 1)
        {
            PushSuggestion(L, "Summary", t->hasSummary ? t->summary : std::string("<Empty Summary>"), "");
            return 1;
        }
        uint32_t i = index - 2;
        if (i < t->commands.size())
        {
            PushSuggestion(L, "Command", t->commands[i].text, t->commands[i].reason);
            return 1;
        }
        i -= static_cast<uint32_t>(t->commands.size());
        if (i < t->sql.size())
        {
            PushSuggestion(L, "SQL", t->sql[i].text, t->sql[i].reason);
            return 1;
        }
        i -= static_cast<uint32_t>(t->sql.size());
        if (i >= t->bugReports.size())
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        PushSuggestion(L, "BugTrackerReport",
                       "https://ascension.gg/bugtracker/view/" + std::to_string(t->bugReports[i].id),
                       t->bugReports[i].reason);
        return 1;
    }

    int IsResponseSeen(lua_State* L)
    {
        std::string id;
        uint32_t messageId;
        if (!Gate() || !ReadStringNumber(L, id, messageId))
            return 0;
        const Ticket* t = FindTicket(id);
        uint8_t* player = t ? ActivePlayer() : nullptr;
        if (!player)
            return 0;
        bool seen = false;
        for (const Message& m : t->messages)
            if (m.messageId == messageId)
                for (const ReadReceipt& r : m.receipts)
                    if (r.readBy == PlayerName(player))
                    {
                        seen = true;
                        break;
                    }
        PushBool(L, seen);
        return 1;
    }

    // No active player reads as assigned.
    int IsTicketAssignedToMe(lua_State* L)
    {
        std::string id;
        if (!Gate() || !ReadString(L, id))
            return 0;
        const Ticket* t = FindTicket(id);
        if (!t)
            return 0;
        bool mine = true;
        if (uint8_t* player = ActivePlayer())
        {
            mine = false;
            for (const std::string& a : t->assignedTo)
                if (a == PlayerName(player))
                    mine = true;
        }
        PushBool(L, mine);
        return 1;
    }

    // SetTicketFilter(search, {FILTER_* = bool}, {SORT_*, ...}) -- FUN_10239fb0, FUN_100bed80,
    // FUN_1011f710. Returns nothing.
    int SetTicketFilter(lua_State* L)
    {
        if (!Gate() || !ValidateInput(L, {STRING, TABLE, TABLE}))
            return 0;
        const std::string search = CheckString(L, 1);
        std::vector<uint32_t> filters, sorts;
        AscLua::lua_pushnil(L);
        while (AscLua::lua_next(L, 2))
        {
            const std::string key = CheckString(L, -2);
            if (AscLua::lua_toboolean(L, -1))
                for (uint32_t i = 0; i < 8; ++i)
                    if (key == kFilters[i])
                    {
                        filters.push_back(i);
                        break;
                    }
            AscLua::lua_settop(L, -2);
        }
        AscLua::lua_pushnil(L);
        while (AscLua::lua_next(L, 3))
        {
            const std::string name = CheckString(L, -1);
            for (uint32_t i = 0; i < 5; ++i)
                if (name == kSorts[i])
                {
                    sorts.push_back(i);
                    break;
                }
            AscLua::lua_settop(L, -2);
        }
        ApplyFilter(search, filters, sorts);
        return 0;
    }

    int CanAssignGMTicket(lua_State* L)   // also CanSetTicketTitle (same function, 0x1024D6D0)
    {
        std::string a, b;
        if (!Gate() || !ReadTwoStrings(L, a, b))
            return 0;
        return PushTrueNil(L);
    }

    int SendTwoStrings(lua_State* L, uint32_t opcode)   // Assign 0x717, Decline 0x714, SetTitle 0x710
    {
        std::string a, b;
        if (!Gate() || !ReadTwoStrings(L, a, b))
            return 0;
        Packet p(opcode);
        p.Str(a.c_str()).Str(b.c_str());
        p.Send();
        PushBool(L, true);
        return 1;
    }
    int AssignGMTicket(lua_State* L) { return SendTwoStrings(L, 0x717); }
    int DeclineAutomatedMessage(lua_State* L) { return SendTwoStrings(L, 0x714); }
    int SetTicketTitle(lua_State* L) { return SendTwoStrings(L, 0x710); }

    int ApproveAutomatedMessage(lua_State* L)
    {
        std::string id;
        if (!Gate() || !ReadString(L, id))
            return 0;
        Packet p(0x712);
        p.Str(id.c_str());
        p.Send();
        PushBool(L, true);
        return 1;
    }

    int CanCloseTicket(lua_State* L)
    {
        bool has;
        std::string id;
        if (!Gate() || !ReadOptId(L, has, id))
            return 0;
        return PushTrueNil(L);
    }

    int CloseTicket(lua_State* L)
    {
        bool has;
        std::string id;
        if (!Gate() || !ReadOptId(L, has, id))
            return 0;
        Packet p(0x705);
        PutOptId(p, has, id);
        p.Send();
        PushBool(L, true);
        return 1;
    }

    int CanMarkResponseSeen(lua_State* L)
    {
        bool has;
        std::string id;
        uint32_t messageId;
        if (!Gate() || !ReadMarkSeen(L, has, id, messageId))
            return 0;
        return PushTrueNil(L);
    }

    int MarkResponseSeen(lua_State* L)
    {
        bool has;
        std::string id;
        uint32_t messageId;
        if (!Gate() || !ReadMarkSeen(L, has, id, messageId))
            return 0;
        Packet p(0x71D);
        PutOptId(p, has, id);
        p.U32(messageId);
        p.Send();
        PushBool(L, true);
        return 1;
    }

    int CanSendTicketMessage(lua_State* L)
    {
        bool has, gmOnly;
        std::string id, text;
        if (!Gate() || !ReadSendMessage(L, has, id, text, gmOnly))
            return 0;
        return PushTrueNil(L);
    }

    int SendTicketMessage(lua_State* L)
    {
        bool has, gmOnly;
        std::string id, text;
        if (!Gate() || !ReadSendMessage(L, has, id, text, gmOnly))
            return 0;
        Packet p(0x707);
        PutOptId(p, has, id);
        p.Str(text.c_str());
        p.U8(gmOnly ? 1 : 0);
        p.Send();
        PushBool(L, true);
        return 1;
    }

    int CanSetTicketPriority(lua_State* L)
    {
        std::string id;
        uint8_t v;
        if (!Gate() || !ReadIdEnum(L, id, v, kPriority, 4, "GMTicketPriority"))
            return 0;
        return PushTrueNil(L);
    }

    int SetTicketPriority(lua_State* L)
    {
        std::string id;
        uint8_t v;
        if (!Gate() || !ReadIdEnum(L, id, v, kPriority, 4, "GMTicketPriority"))
            return 0;
        Packet p(0x70E);
        p.Str(id.c_str()).U8(v);
        p.Send();
        PushBool(L, true);
        return 1;
    }

    int CanSetTicketStatus(lua_State* L)
    {
        std::string id;
        uint8_t v;
        if (!Gate() || !ReadIdEnum(L, id, v, kStatus, 7, "GMTicketStatus"))
            return 0;
        return PushTrueNil(L);
    }

    int SetTicketStatus(lua_State* L)
    {
        std::string id;
        uint8_t v;
        if (!Gate() || !ReadIdEnum(L, id, v, kStatus, 7, "GMTicketStatus"))
            return 0;
        Packet p(0x70C);
        p.Str(id.c_str()).U8(v);
        p.Send();
        PushBool(L, true);
        return 1;
    }

    // ---- C_PlayerTicket -------------------------------------------------------------------------
    int GetCurrentTicket(lua_State* L)
    {
        if (!Gate() || !g_hasCurrent)
            return 0;
        PushTicket(L, g_current);
        return 1;
    }

    int CanCreateTicket(lua_State* L)
    {
        uint8_t priority, category;
        std::string a, b, c;
        if (!Gate() || !ReadCreate(L, priority, category, a, b, c))
            return 0;
        return PushTrueNil(L);
    }

    int CreateTicket(lua_State* L)
    {
        uint8_t priority, category;
        std::string a, b, c;
        if (!Gate() || !ReadCreate(L, priority, category, a, b, c))
            return 0;
        Packet p(0x703);
        p.U8(priority).U8(category).Str(a.c_str()).Str(b.c_str()).Str(c.c_str());
        p.Send();
        PushBool(L, true);
        return 1;
    }

    // An existing ticket's id always replaces the argument.
    bool ReopenId(lua_State* L, bool& has, std::string& id)
    {
        if (!ReadOptId(L, has, id))
            return false;
        if (g_hasCurrent)
        {
            id = g_current.ticketId;
            has = true;
        }
        return true;
    }

    int CanReopenTicket(lua_State* L)
    {
        bool has;
        std::string id;
        if (!Gate() || !ReopenId(L, has, id))
            return 0;
        return PushTrueNil(L);
    }

    int ReopenTicket(lua_State* L)
    {
        bool has;
        std::string id;
        if (!Gate() || !ReopenId(L, has, id))
            return 0;
        Packet p(0x719);
        PutOptId(p, has, id);
        p.Send();
        PushBool(L, true);
        return 1;
    }

    // C_GM.RequestPlayerInfo(name) (0x1024ffe0): CMSG 0x71B with the name; the answer is SMSG 0x71C.
    int RequestPlayerInfo(lua_State* L)
    {
        std::string name;
        if (!Gate() || !ReadString(L, name))
            return 0;
        Packet(0x71B).Str(name.c_str()).Send();
        PushBool(L, true);
        return 1;
    }

    // ---- C_GM.GetPlayerInfo -----------------------------------------------------------------------
    void SetF32(lua_State* L, const char* k, bool has, float v)   // FUN_1009b2b0 = pushnumber((double)f)
    {
        AscLua::lua_pushstring(L, k);
        if (has) AscLua::lua_pushnumber(L, static_cast<double>(v)); else AscLua::lua_pushnil(L);
        AscLua::lua_settable(L, -3);
    }
    // Vector writers: createtable(0, n), each entry keyed with lua_pushnumber(i).
    template <class T, class F> void SetList(lua_State* L, const char* k, const std::vector<T>& v, int fields, F write)
    {
        AscLua::lua_pushstring(L, k);
        AscLua::lua_createtable(L, 0, static_cast<int>(v.size()));
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < v.size(); ++i)
        {
            BeginEntry(L, i + 1);
            AscLua::lua_createtable(L, 0, fields);
            AscLua::lua_checkstack(L, 2);
            write(v[i]);
            AscLua::lua_settable(L, -3);
        }
        AscLua::lua_settable(L, -3);
    }

    void PushPlayerInfo(lua_State* L, const PlayerInfo& r)   // FUN_10247d40
    {
        AscLua::lua_createtable(L, 0, 0x25);
        AscLua::lua_checkstack(L, 2);
        SetStr(L, "CharacterName", r.name);
        SetU32(L, "CharacterGuid", r.guid);
        SetU32(L, "CharacterLevel", r.level);
        SetU32(L, "RaceID", r.race);
        SetU32(L, "ClassID", r.classId);
        SetU32(L, "GameModeMask", r.gameModeMask);
        SetF32(L, "AverageItemLevel", r.hasItemLevel, r.itemLevel);
        SetOptU32(L, "Money", r.hasMoney, r.money);
        SetList(L, "Challenges", r.challenges, 2, [L](const std::pair<uint32_t, uint32_t>& c) {   // FUN_10240e30
            SetU32(L, "ChallengeID", c.first);
            SetU32(L, "ChallengeLevel", c.second);
        });
        SetOptU32(L, "MapID", r.hasMap, r.map);
        SetF32(L, "LocX", r.hasX, r.x);
        SetF32(L, "LocY", r.hasY, r.y);
        SetF32(L, "LocZ", r.hasZ, r.z);
        SetOptU32(L, "ZoneID", r.hasZone, r.zone);
        SetOptU32(L, "AreaID", r.hasArea, r.area);
        SetU32(L, "GuildID", r.guildId);
        SetOptStr(L, "GuildName", r.hasGuildName, r.guildName);
        SetOptU32(L, "GuildRankID", r.hasRank, r.rank);
        SetOptStr(L, "GuildRankName", r.hasRankName, r.rankName);
        SetU32(L, "AccountID", r.accountId);
        SetStr(L, "AccountName", r.accountName);
        AscLua::lua_pushstring(L, "GMLevel");   // FUN_10241430
        if (r.gmLevel < 8)
            AscLua::lua_pushstring(L, kSecurity[r.gmLevel]);
        else
            AscLua::lua_pushstring(L, ("UNEXPECTED_ENUM_VALUE_" + std::to_string(r.gmLevel)).c_str());
        AscLua::lua_settable(L, -3);
        SetOptStr(L, "DiscordName", r.hasDiscord, r.discord);
        SetOptStr(L, "HardwareID", r.hasHwid, r.hwid);
        SetOptStr(L, "HardwareIDv2", r.hasHwid2, r.hwid2);
        SetOptStr(L, "OnlineCharacterName", r.hasOnlineName, r.onlineName);
        SetList(L, "TicketHistory", r.tickets, 5, [L](const HistTicket& t) {   // FUN_10241290 / FUN_10243010
            SetStr(L, "TicketID", t.ticketId);
            SetStr(L, "Title", t.title);
            SetU32(L, "CreationDate", t.creationDate);
            SetOptU32(L, "CloseDate", t.hasCloseDate, t.closeDate);
            SetList(L, "Messages", t.messages, 5, [L](const HistMessage& m) {   // FUN_102405a0 / FUN_10243340
                SetBool(L, "IsFromGM", m.isFromGM);
                SetBool(L, "GMOnly", m.gmOnly);
                SetStr(L, "Sender", m.sender);
                SetOptStr(L, "Message", m.hasMessage, m.message);
                SetU32(L, "Timestamp", m.timestamp);
            });
        });
        SetU32(L, "NumTickets", r.numTickets);
        SetList(L, "MuteHistory", r.mutes, 6, [L](const Mute& m) {   // FUN_10240f60 / FUN_102425c0
            SetU32(L, "MuteDate", m.date);
            SetU32(L, "MuteTime", m.time);
            SetStr(L, "MuteReason", m.reason);
            SetStr(L, "MutedBy", m.by);
            SetU32(L, "MuteMode", m.mode);
            SetStr(L, "RealmName", m.realm);
        });
        SetU32(L, "NumMutes", r.numMutes);
        SetList(L, "BanHistory", r.bans, 6, [L](const Ban& b) {   // FUN_10240cd0 / FUN_10242020
            SetU32(L, "BanDate", b.date);
            SetU32(L, "RemainingTime", b.remaining);
            SetBool(L, "IsActive", b.active);
            SetU32(L, "UnbanDate", b.unbanDate);
            SetStr(L, "BanReason", b.reason);
            SetStr(L, "BannedBy", b.by);
        });
        SetU32(L, "NumBans", r.numBans);
        SetList(L, "WarningHistory", r.warnings, 4, [L](const Warning& w) {   // FUN_10241100 / FUN_10242970
            SetStr(L, "RealmName", w.realm);
            SetStr(L, "CharacterName", w.character);
            SetStr(L, "Reason", w.reason);
            SetU32(L, "Timestamp", w.timestamp);
        });
        SetU32(L, "NumWarnings", r.numWarnings);
        SetList(L, "GMNotes", r.notes, 4, [L](const Note& n) {   // FUN_1023fce0
            SetU32(L, "NoteID", n.id);
            SetStr(L, "GM", n.gm);
            SetStr(L, "Message", n.message);
            SetU32(L, "Date", n.date);
        });
        SetU32(L, "NumNotes", r.numNotes);
        SetOptU32(L, "Latency", r.hasLatency, r.latency);
    }

    // C_GM.GetPlayerInfo(name) (0x1024fe40): the FIRST stored record whose name matches exactly
    // (FUN_10087260), or nil.
    int GetPlayerInfo(lua_State* L)
    {
        std::string name;
        if (!Gate() || !ReadString(L, name))
            return 0;
        for (const PlayerInfo& r : g_playerInfo)
            if (r.name == name)
            {
                PushPlayerInfo(L, r);
                return 1;
            }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // The player-side twins of the C_GMTicket calls (0x10255330..0x10256640): the same readers, with the
    // player's own ticket id replacing the argument whenever there is a ticket.
    void UseOwnTicket(bool& has, std::string& id)
    {
        if (g_hasCurrent)
        {
            id = g_current.ticketId;
            has = true;
        }
    }

    int PlayerCanCloseTicket(lua_State* L)   // 0x10255330
    {
        bool has;
        std::string id;
        if (!Gate() || !ReadOptId(L, has, id))
            return 0;
        UseOwnTicket(has, id);
        return PushTrueNil(L);
    }

    int PlayerCloseTicket(lua_State* L)   // 0x10255840
    {
        bool has;
        std::string id;
        if (!Gate() || !ReadOptId(L, has, id))
            return 0;
        UseOwnTicket(has, id);
        Packet p(0x705);
        PutOptId(p, has, id);
        p.Send();
        PushBool(L, true);
        return 1;
    }

    int PlayerCanSendTicketMessage(lua_State* L)   // 0x10255750 (reader FUN_10239ab0 -> FUN_1023a2e0)
    {
        bool has, gmOnly;
        std::string id, text;
        if (!Gate() || !ReadSendMessage(L, has, id, text, gmOnly))
            return 0;
        UseOwnTicket(has, id);
        return PushTrueNil(L);
    }

    int PlayerSendTicketMessage(lua_State* L)   // 0x10256640
    {
        bool has, gmOnly;
        std::string id, text;
        if (!Gate() || !ReadSendMessage(L, has, id, text, gmOnly))
            return 0;
        UseOwnTicket(has, id);
        Packet p(0x707);
        PutOptId(p, has, id);
        p.Str(text.c_str());
        p.U8(gmOnly ? 1 : 0);
        p.Send();
        PushBool(L, true);
        return 1;
    }

    int PlayerCanMarkResponseSeen(lua_State* L)   // 0x10255530
    {
        bool has;
        std::string id;
        uint32_t messageId;
        if (!Gate() || !ReadMarkSeen(L, has, id, messageId))
            return 0;
        UseOwnTicket(has, id);
        return PushTrueNil(L);
    }

    int PlayerMarkResponseSeen(lua_State* L)   // 0x10256200
    {
        bool has;
        std::string id;
        uint32_t messageId;
        if (!Gate() || !ReadMarkSeen(L, has, id, messageId))
            return 0;
        UseOwnTicket(has, id);
        Packet p(0x71D);
        PutOptId(p, has, id);
        p.U32(messageId);
        p.Send();
        PushBool(L, true);
        return 1;
    }

    // 0x10255e20: the player's ticket messages without the GM-only ones; nothing without a ticket.
    int PlayerGetTicketMessages(lua_State* L)
    {
        if (!Gate() || !g_hasCurrent)
            return 0;
        std::vector<Message> v;
        for (const Message& m : g_current.messages)
            if (!m.gmOnly)
                v.push_back(m);
        PushMessages(L, v);
        return 1;
    }

    // 0x10255d70. The original inverts its reader check: a readable message id returns nothing, and an
    // unreadable one searches with whatever the (uninitialised) id holds -- 0 here.
    int PlayerGetTicketMessage(lua_State* L)
    {
        if (!Gate())
            return 0;
        uint32_t messageId = 0;
        if (ReadNumber(L, messageId))
            return 0;
        messageId = 0;
        if (!g_hasCurrent)
            return 0;
        for (const Message& m : g_current.messages)
            if (m.messageId == messageId && !m.gmOnly)
            {
                PushMessage(L, m);
                return 1;
            }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // 0x10256030: has the player (by name) a read receipt on the message?
    int PlayerIsResponseSeen(lua_State* L)
    {
        uint32_t messageId;
        uint8_t* player;
        if (!Gate() || !ReadNumber(L, messageId) || !g_hasCurrent || !(player = ActivePlayer()))
            return 0;
        typedef const char*(__thiscall * GetName_t)(void*);
        bool seen = false;
        for (const Message& m : g_current.messages)
        {
            if (m.messageId != messageId)
                continue;
            for (const ReadReceipt& r : m.receipts)
            {
                const char* name = (*reinterpret_cast<GetName_t**>(player))[0xD8 / 4](player);
                if (r.readBy == (name ? name : ""))
                {
                    seen = true;
                    break;
                }
            }
            if (seen)
                break;
        }
        PushBool(L, seen);
        return 1;
    }

    // ---- packet handlers ------------------------------------------------------------------------
    // The managers' diagnostics go through the original's timestamped channel writer (FUN_102936d0):
    // 1 Info for changes, 2 Debug for progress and fired events, 4 Error for an unknown ticket.
    void Diag(uint32_t channel, const char* fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        va_list copy;
        va_copy(copy, ap);
        const int n = vsnprintf(nullptr, 0, fmt, copy);
        va_end(copy);
        std::string line(n > 0 ? static_cast<size_t>(n) : 0, '\0');
        if (n > 0)
            vsnprintf(&line[0], static_cast<size_t>(n) + 1, fmt, ap);
        va_end(ap);
        AscLogger::WriteStamped(channel, line);
    }

    void __cdecl OnTicketList(void*, uint32_t, uint32_t, CDataStore* p)   // 0x709
    {
        if (Read<uint8_t>(p))
            g_tickets.clear();
        const bool reset = Read<uint8_t>(p) != 0;
        for (uint32_t n = Read<uint32_t>(p); n; --n)
            g_tickets.push_back(ReadTicket(p));
        if (reset)
            ResetView();
    }

    void __cdecl OnTicketCreated(void*, uint32_t, uint32_t, CDataStore* p)   // 0x70A
    {
        g_tickets.push_back(ReadTicket(p));
        AscRuntime::Signal("GM_TICKET_NEW_TICKET", "%s", g_tickets.back().ticketId.c_str());
        AscRuntime::Signal("GM_TICKETS_UPDATED");
        ResetView();
        AscRuntime::Signal("GM_TICKET_FILTER_RESET");
    }

    void __cdecl OnTicketUpdate(void*, uint32_t, uint32_t, CDataStore* p)   // 0x70B (FUN_1024af30)
    {
        static const char kTag[] = "GMTicketMgr::HandleGMTicketUpdateOpcode: ";
        Diag(2, "%sBegin processing ticket update", kTag);
        Ticket fresh = ReadTicket(p);
        const char* id = fresh.ticketId.c_str();
        Diag(2, "%sProcessing ticket ID: %s", kTag, id);
        Ticket* slot = FindTicket(fresh.ticketId);
        if (!slot)
        {
            Diag(4, "%sTicket %s not found", kTag, id);
            return;
        }
        const Ticket old = *slot;
        *slot = fresh;
        Diag(2, "%sFound and updated existing ticket: %s", kTag, id);

        bool statusChanged = false;
        if (old.status != fresh.status)
        {
            Diag(1, "%sTicket %s status changed from %d to %d", kTag, id, old.status, fresh.status);
            if (old.Closed() != fresh.Closed())
            {
                Diag(1, "%sTicket %s closure state changed: %s -> %s", kTag, id, old.Closed() ? "Closed" : "Open",
                     fresh.Closed() ? "Closed" : "Open");
                AscRuntime::Signal("GM_TICKETS_UPDATED");
                Diag(2, "%sFired GM_TICKETS_UPDATED event", kTag);
            }
            statusChanged = true;
        }
        if (old.assignedTo.size() != fresh.assignedTo.size())
        {
            Diag(1, "%sTicket %s assignment count changed: %u -> %u", kTag, id,
                 static_cast<uint32_t>(old.assignedTo.size()), static_cast<uint32_t>(fresh.assignedTo.size()));
            if (!fresh.assignedTo.empty())
            {
                std::string names;
                for (const std::string& n : fresh.assignedTo)
                {
                    if (!names.empty())
                        names += ", ";
                    names += n;
                }
                Diag(1, "%sTicket %s assigned to: %s", kTag, id, names.c_str());
            }
        }
        if (old.assignedTo.size() != fresh.assignedTo.size() || statusChanged)
        {
            AscRuntime::Signal("GM_TICKET_STATUS_CHANGED", "%s", id);
            Diag(2, "%sFired GM_TICKET_STATUS_CHANGED event for ticket %s", kTag, id);
        }
        if (old.messages.size() != fresh.messages.size())
        {
            Diag(1, "%sTicket %s message count changed: %u -> %u", kTag, id,
                 static_cast<uint32_t>(old.messages.size()), static_cast<uint32_t>(fresh.messages.size()));
            if (old.messages.size() < fresh.messages.size())
                Diag(1, "%sNew message (ID: %u) from: %s", kTag, fresh.messages.back().messageId,
                     fresh.messages.back().sender.c_str());
            AscRuntime::Signal("GM_TICKET_MESSAGE", "%s", id);
            Diag(2, "%sFired GM_TICKET_MESSAGE event for ticket %s", kTag, id);
        }
        if (old.hasAISuggestion != fresh.hasAISuggestion
            || (old.hasAISuggestion && old.aiSuggestion != fresh.aiSuggestion))
        {
            Diag(1, "%sTicket %s AI suggestion changed", kTag, id);
            AscRuntime::Signal("GM_TICKET_SUGGESTION_UPDATE", "%s", id);
            Diag(2, "%sFired GM_TICKET_SUGGESTION_UPDATE event for ticket %s", kTag, id);
        }
        Diag(2, "%sChecking read receipts for %u messages in ticket %s", kTag,
             static_cast<uint32_t>(fresh.messages.size()), id);
        for (size_t i = 0; i < fresh.messages.size() && i < old.messages.size(); ++i)
        {
            const Message& m = fresh.messages[i];
            const size_t had = old.messages[i].receipts.size();
            if (m.receipts.size() <= had)
                continue;
            Diag(1, "%sMessage %u in ticket %s has new read receipts: %u -> %u", kTag, m.messageId, id,
                 static_cast<uint32_t>(had), static_cast<uint32_t>(m.receipts.size()));
            for (size_t j = had; j < m.receipts.size(); ++j)
                Diag(1, "%sMessage %u in ticket %s read by: %s", kTag, m.messageId, id, m.receipts[j].readBy.c_str());
            for (size_t j = had; j < m.receipts.size(); ++j)
            {
                AscRuntime::Signal("GM_TICKET_MESSAGE_READ", "%s%u", id, m.messageId);
                Diag(2, "%sFired GM_TICKET_MESSAGE_READ event for message %u in ticket %s", kTag, m.messageId, id);
            }
        }
        ResetView();
        AscRuntime::Signal("GM_TICKET_FILTER_RESET");
        Diag(2, "%sFired GM_TICKET_FILTER_RESET event", kTag);
        Diag(2, "%sFinished processing ticket update", kTag);
    }

    void __cdecl OnPlayerInfoResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x71C
    {
        const std::string name = ReadStr(p);
        if (!Read<uint8_t>(p))
        {
            AscRuntime::Signal("REQUEST_PLAYER_INFO_RESULT", "%s", name.c_str());
            return;
        }
        g_playerInfo.push_back(ReadPlayerInfo(p));   // FUN_1023a4a0 -> FUN_102472d0
        AscRuntime::Signal("REQUEST_PLAYER_INFO_RESULT", "%s%s", g_playerInfo.back().name.c_str(), name.c_str());
    }

    void __cdecl OnPlayerTicketInfo(void*, uint32_t, uint32_t, CDataStore* p)   // 0x701
    {
        g_current = ReadTicket(p);
        g_hasCurrent = true;
        AscRuntime::Signal("PLAYER_TICKET_UPDATE");
    }

    void __cdecl OnPlayerTicketUpdate(void*, uint32_t, uint32_t, CDataStore* p)   // 0x702 (FUN_10253ec0)
    {
        static const char kTag[] = "PlayerTicketMgr::HandlePlayerGMTicketUpdateOpcode: ";
        Diag(2, "%sBegin processing player ticket update", kTag);
        const bool had = g_hasCurrent;
        const Ticket old = g_current;
        g_current = ReadTicket(p);
        g_hasCurrent = true;
        // The original's "No ticket in update" branch cannot run: the read always leaves a ticket.
        Diag(2, "%sProcessing player ticket ID: %s", kTag, g_current.ticketId.c_str());

        bool fire = false;
        if (had)
        {
            Diag(2, "%sBoth old and new tickets exist, checking changes", kTag);
            // A hidden last message in the old ticket suppresses the message-count checks.
            const bool hidden = !old.messages.empty() && !old.messages.back().hasMessage;
            if (hidden)
                Diag(2, "%sLast message in old ticket was hidden", kTag);
            const bool statusChanged = old.status != g_current.status;
            if (statusChanged)
                Diag(1, "%sTicket status changed from %d to %d", kTag, old.status, g_current.status);
            const bool countChanged = !hidden && old.messages.size() != g_current.messages.size();
            if (countChanged)
                Diag(1, "%sMessage count changed: %u -> %u (last hidden: no)", kTag,
                     static_cast<uint32_t>(old.messages.size()), static_cast<uint32_t>(g_current.messages.size()));
            fire = statusChanged || countChanged;
            if (fire)
                Diag(1, "%sWill send update event due to: status changed=%s, messages changed=%s", kTag,
                     statusChanged ? "true" : "false", countChanged ? "true" : "false");
            if (!hidden && old.messages.size() < g_current.messages.size())
            {
                const Message& last = g_current.messages.back();
                Diag(1, "%sNew message added (ID: %u) from: %s", kTag, last.messageId, last.sender.c_str());
                AscRuntime::Signal("PLAYER_TICKET_NEW_MESSAGE", "%u", last.messageId);
                Diag(2, "%sFired PLAYER_TICKET_NEW_MESSAGE event for message ID %u", kTag, last.messageId);
            }
            if (g_current.closedByCreator)
            {
                Diag(1, "%sClearing ticket %s because it's closed by creator", kTag, g_current.ticketId.c_str());
                g_hasCurrent = false;   // the player closed it: forget it
                fire = true;
            }
        }
        else if (!g_current.closedByCreator)
        {
            Diag(1, "%sGM reopened a ticket previously closed by player", kTag);
            fire = true;                // a GM reopened a ticket the player had closed
        }
        if (fire)
        {
            AscRuntime::Signal("PLAYER_TICKET_UPDATE");
            Diag(2, "%sFired PLAYER_TICKET_UPDATE event", kTag);
        }
        else
            Diag(2, "%sNo PLAYER_TICKET_UPDATE event needed", kTag);
        Diag(2, "%sFinished processing player ticket update", kTag);
    }

    // The result handlers of both managers: a C string, signalled as the event's "%s". 0x706 / 0x71E pick
    // the event by the account's GM level (AccountInfo +8, FUN_1008c870) -- note 0x706 names the PLAYER
    // result for GMs and 0x71E the GM one, as in the original.
    std::string ResultStr(CDataStore* p)
    {
        std::string s(reinterpret_cast<const char*>(p->m_buffer + p->m_read));
        p->m_read += static_cast<int32_t>(s.size() + 1);
        return s;
    }
    void SignalResult(const char* event, CDataStore* p) { const std::string s = ResultStr(p); AscRuntime::Signal(event, "%s", s.c_str()); }
    void __cdecl On704(void*, uint32_t, uint32_t, CDataStore* p) { SignalResult("CREATE_PLAYER_TICKET_RESULT", p); }      // FUN_10253a30
    void __cdecl On70D(void*, uint32_t, uint32_t, CDataStore* p) { SignalResult("UPDATE_GM_TICKET_STATUS_RESULT", p); }   // FUN_1024cca0
    void __cdecl On70F(void*, uint32_t, uint32_t, CDataStore* p) { SignalResult("UPDATE_GM_TICKET_PRIORITY_RESULT", p); } // FUN_1024cb60
    void __cdecl On711(void*, uint32_t, uint32_t, CDataStore* p) { SignalResult("UPDATE_GM_TICKET_TITLE_RESULT", p); }    // FUN_1024ce00
    void __cdecl On713(void*, uint32_t, uint32_t, CDataStore* p) { SignalResult("APPROVE_GM_TICKET_SUGGESTION_RESULT", p); } // FUN_1024a690
    void __cdecl On715(void*, uint32_t, uint32_t, CDataStore* p) { SignalResult("DECLINE_GM_TICKET_SUGGESTION_RESULT", p); } // FUN_1024a940
    void __cdecl On718(void*, uint32_t, uint32_t, CDataStore* p) { SignalResult("ASSIGN_GM_TICKET_RESULT", p); }          // FUN_1024a7f0
    void __cdecl On71A(void*, uint32_t, uint32_t, CDataStore* p) { SignalResult("REOPEN_PLAYER_TICKET_RESULT", p); }      // FUN_10254c90
    // 0x708 (FUN_10254df0): result, u8 has-message [message, FUN_1023a7d0 -> FUN_102434c0]. A GM account
    // gets SEND_PLAYER_TICKET_MESSAGE_RESULT("%s%u", result, message id); otherwise
    // SEND_GM_TICKET_MESSAGE_RESULT("%s%s%u", result, ticket id, message id) -- "" / 0 without a message.
    void __cdecl On708(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const std::string result = ResultStr(p);
        Message m;
        const bool has = Read<uint8_t>(p) != 0;
        if (has)
            m = ReadMessage(p);
        if (AscAccount::GmLevel() >= 1)
            AscRuntime::Signal("SEND_PLAYER_TICKET_MESSAGE_RESULT", "%s%u", result.c_str(), has ? m.messageId : 0u);
        else
            AscRuntime::Signal("SEND_GM_TICKET_MESSAGE_RESULT", "%s%s%u", result.c_str(), has ? m.ticketId.c_str() : "",
                               has ? m.messageId : 0u);
    }
    void __cdecl On706(void*, uint32_t, uint32_t, CDataStore* p)   // FUN_10253800
    {
        SignalResult(AscAccount::GmLevel() >= 1 ? "CLOSE_PLAYER_TICKET_RESULT" : "CLOSE_GM_TICKET_RESULT", p);
    }
    void __cdecl On71E(void*, uint32_t, uint32_t, CDataStore* p)   // FUN_10253b90
    {
        SignalResult(AscAccount::GmLevel() >= 1 ? "MARK_GM_TICKET_MESSAGE_AS_READ_RESULT" : "MARK_PLAYER_TICKET_MESSAGE_AS_READ_RESULT", p);
    }

    // FUN_10249940 (every glue screen): the ticket list emptied, the container view reset (FUN_1024d1f0)
    // and the player-info results (+0x88) dropped.
    void ResetTickets()
    {
        g_tickets.clear();
        ResetView();
        g_playerInfo.clear();
    }
    // FUN_102537d0 (every glue screen): the player's own ticket released when there is one.
    void ResetCurrent()
    {
        if (!g_hasCurrent)
            return;
        g_current = Ticket{};
        g_hasCurrent = false;
    }

    void Init()
    {
        AscRuntime::OnGlueScreen(&ResetTickets);
        AscRuntime::OnGlueScreen(&ResetCurrent);
        sDC.AddPacketHandler(0x701, CNetClientCustomPacket((void*)&OnPlayerTicketInfo, nullptr));
        sDC.AddPacketHandler(0x702, CNetClientCustomPacket((void*)&OnPlayerTicketUpdate, nullptr));
        sDC.AddPacketHandler(0x709, CNetClientCustomPacket((void*)&OnTicketList, nullptr));
        sDC.AddPacketHandler(0x70A, CNetClientCustomPacket((void*)&OnTicketCreated, nullptr));
        sDC.AddPacketHandler(0x70B, CNetClientCustomPacket((void*)&OnTicketUpdate, nullptr));
        sDC.AddPacketHandler(0x71C, CNetClientCustomPacket((void*)&OnPlayerInfoResult, nullptr));
        sDC.AddPacketHandler(0x704, CNetClientCustomPacket((void*)&On704, nullptr));
        sDC.AddPacketHandler(0x706, CNetClientCustomPacket((void*)&On706, nullptr));
        sDC.AddPacketHandler(0x708, CNetClientCustomPacket((void*)&On708, nullptr));
        sDC.AddPacketHandler(0x70D, CNetClientCustomPacket((void*)&On70D, nullptr));
        sDC.AddPacketHandler(0x70F, CNetClientCustomPacket((void*)&On70F, nullptr));
        sDC.AddPacketHandler(0x711, CNetClientCustomPacket((void*)&On711, nullptr));
        sDC.AddPacketHandler(0x713, CNetClientCustomPacket((void*)&On713, nullptr));
        sDC.AddPacketHandler(0x715, CNetClientCustomPacket((void*)&On715, nullptr));
        sDC.AddPacketHandler(0x718, CNetClientCustomPacket((void*)&On718, nullptr));
        sDC.AddPacketHandler(0x71A, CNetClientCustomPacket((void*)&On71A, nullptr));
        sDC.AddPacketHandler(0x71E, CNetClientCustomPacket((void*)&On71E, nullptr));
    }

    const AscBindings::Binding kBindings[] = {
        {"C_GMTicket", "ApproveAutomatedMessage", ApproveAutomatedMessage},
        {"C_GMTicket", "AssignGMTicket", AssignGMTicket},
        {"C_GMTicket", "CanAssignGMTicket", CanAssignGMTicket},
        {"C_GMTicket", "CanCloseTicket", CanCloseTicket},
        {"C_GMTicket", "CanMarkResponseSeen", CanMarkResponseSeen},
        {"C_GMTicket", "CanSendTicketMessage", CanSendTicketMessage},
        {"C_GMTicket", "CanSetTicketPriority", CanSetTicketPriority},
        {"C_GMTicket", "CanSetTicketStatus", CanSetTicketStatus},
        {"C_GMTicket", "CanSetTicketTitle", CanAssignGMTicket},
        {"C_GMTicket", "CloseTicket", CloseTicket},
        {"C_GMTicket", "DeclineAutomatedMessage", DeclineAutomatedMessage},
        {"C_GMTicket", "GetAutomatedMessage", GetAutomatedMessage},
        {"C_GMTicket", "GetNumSuggestions", GetNumSuggestions},
        {"C_GMTicket", "GetNumTickets", GetNumTickets},
        {"C_GMTicket", "GetSuggestionAtIndex", GetSuggestionAtIndex},
        {"C_GMTicket", "GetTicketAtIndex", GetTicketAtIndex},
        {"C_GMTicket", "GetTicketByID", GetTicketByID},
        {"C_GMTicket", "GetTicketMessage", GetTicketMessage},
        {"C_GMTicket", "GetTicketMessages", GetTicketMessages},
        {"C_GMTicket", "IsResponseSeen", IsResponseSeen},
        {"C_GMTicket", "IsTicketAssignedToMe", IsTicketAssignedToMe},
        {"C_GMTicket", "MarkResponseSeen", MarkResponseSeen},
        {"C_GMTicket", "SendTicketMessage", SendTicketMessage},
        {"C_GMTicket", "SetTicketFilter", SetTicketFilter},
        {"C_GMTicket", "SetTicketPriority", SetTicketPriority},
        {"C_GMTicket", "SetTicketStatus", SetTicketStatus},
        {"C_GMTicket", "SetTicketTitle", SetTicketTitle},
        {"C_PlayerTicket", "CanCreateTicket", CanCreateTicket},
        {"C_PlayerTicket", "CanReopenTicket", CanReopenTicket},
        {"C_PlayerTicket", "CreateTicket", CreateTicket},
        {"C_PlayerTicket", "GetCurrentTicket", GetCurrentTicket},
        {"C_PlayerTicket", "ReopenTicket", ReopenTicket},
        {"C_PlayerTicket", "CanCloseTicket", PlayerCanCloseTicket},
        {"C_PlayerTicket", "CloseTicket", PlayerCloseTicket},
        {"C_PlayerTicket", "CanSendTicketMessage", PlayerCanSendTicketMessage},
        {"C_PlayerTicket", "SendTicketMessage", PlayerSendTicketMessage},
        {"C_PlayerTicket", "CanMarkResponseSeen", PlayerCanMarkResponseSeen},
        {"C_PlayerTicket", "MarkResponseSeen", PlayerMarkResponseSeen},
        {"C_PlayerTicket", "GetTicketMessages", PlayerGetTicketMessages},
        {"C_PlayerTicket", "GetTicketMessage", PlayerGetTicketMessage},
        {"C_PlayerTicket", "IsResponseSeen", PlayerIsResponseSeen},
        {"C_GM", "GetPlayerInfo", GetPlayerInfo},
        {"C_GM", "RequestPlayerInfo", RequestPlayerInfo},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}
