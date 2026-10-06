// C_PlayerPoll over PlayerPollMgr (FUN_102d8590 -> static 0x10BE37DC): questions (+0) and the
// has-unanswered flag (+0xC, FUN_102d85f0).
//
// Question on the wire (FUN_102d6f80): u32 id, u32, str text, str, str, u32, u32, u32 n x {u32 id, str
// text} choices, u8 answered [u32 choice id], optional str feedback (u8 flag + str), u8 changeable,
// optional str, u32, u32 type (0..4, PLAYER_POLL_*).
//   SMSG_PLAYER_POLL_LIST 0x746: u32 n x question, replacing the list.
//   SMSG_PLAYER_POLL_SUBMIT_RESULT 0x748: str result, then (if bytes remain) u8 present + question,
//     which replaces the question with the same id or is appended. PLAYER_POLL_ANSWER_RESULT("%s%u",
//     result, poll id or 0).
// Either packet fires PLAYER_POLL_NEW_POLL("%u", first unanswered id) when the flag goes 0 -> 1.
// RequestQuestionList: CMSG 0x745. SubmitAnswer: CMSG 0x747 {u32 question id, u8 has choice
// [u32 choice id], u8 has feedback [str feedback]}.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscLogger.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace AscScript;

namespace
{
    struct Choice { uint32_t id = 0; std::string text; };
    struct Question
    {
        uint32_t id = 0, unk4 = 0;
        std::string text, str20, str38;
        uint32_t unk50 = 0, unk54 = 0;
        std::vector<Choice> choices;
        bool answered = false;
        uint32_t answer = 0;
        bool hasFeedback = false;
        std::string feedback;
        bool changeable = false;
        bool hasStr8C = false;
        std::string str8C;
        uint32_t unkA8 = 0, type = 0;

        // FUN_102d85f0 / FUN_102d7b30's "answered" test.
        bool Done() const { return answered || (type == 4 && hasFeedback && !feedback.empty()); }
    };

    std::vector<Question> g_questions;
    bool g_unanswered = false;
    const uint32_t kMaxFeedback = 0x800;

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

    Question ReadQuestion(CDataStore* p)
    {
        Question q;
        q.id = Read<uint32_t>(p);
        q.unk4 = Read<uint32_t>(p);
        q.text = ReadStr(p);
        q.str20 = ReadStr(p);
        q.str38 = ReadStr(p);
        q.unk50 = Read<uint32_t>(p);
        q.unk54 = Read<uint32_t>(p);
        for (uint32_t n = Read<uint32_t>(p); n; --n)   // FUN_102d6740
        {
            Choice c;
            c.id = Read<uint32_t>(p);
            c.text = ReadStr(p);
            q.choices.push_back(c);
        }
        q.answered = Read<uint8_t>(p) != 0;
        if (q.answered)
            q.answer = Read<uint32_t>(p);
        q.hasFeedback = ReadOptStr(p, q.feedback);
        q.changeable = Read<uint8_t>(p) != 0;
        q.hasStr8C = ReadOptStr(p, q.str8C);
        q.unkA8 = Read<uint32_t>(p);
        q.type = Read<uint32_t>(p);
        return q;
    }

    void Recompute(bool before)
    {
        g_unanswered = false;
        for (const Question& q : g_questions)
            if (!q.Done())
            {
                g_unanswered = true;
                break;
            }
        if (!before && g_unanswered)
        {
            uint32_t first = 0;
            for (const Question& q : g_questions)
                if (!q.Done())
                {
                    first = q.id;
                    break;
                }
            AscRuntime::Signal("PLAYER_POLL_NEW_POLL", "%u", first);
        }
    }

    void __cdecl OnPollList(void*, uint32_t opcode, uint32_t, CDataStore* p)   // FUN_102d7d90
    {
        const bool before = g_unanswered;
        std::vector<Question> list;
        for (uint32_t n = Read<uint32_t>(p); n; --n)
            list.push_back(ReadQuestion(p));
        g_questions.swap(list);
        Recompute(before);
        char line[128];
        snprintf(line, sizeof(line), "PlayerPollMgr::HandlePlayerPollListOpcode: Received %u polls for opcode %u",
                 static_cast<uint32_t>(g_questions.size()), opcode);
        AscLogger::WriteStamped(2, line);   // FUN_102936d0, Debug
    }

    void __cdecl OnSubmitResult(void*, uint32_t opcode, uint32_t, CDataStore* p)   // FUN_102d8030
    {
        const std::string result = ReadStr(p);
        uint32_t pollId = 0;
        const bool before = g_unanswered;
        if (p->m_read < p->m_size && Read<uint8_t>(p))
        {
            const Question q = ReadQuestion(p);
            pollId = q.id;
            bool replaced = false;
            for (Question& e : g_questions)
                if (e.id == q.id)
                {
                    e = q;
                    replaced = true;
                    break;
                }
            if (!replaced)
                g_questions.push_back(q);
            Recompute(before);
        }
        else
            Recompute(true);
        AscRuntime::Signal("PLAYER_POLL_ANSWER_RESULT", "%s%u", result.c_str(), pollId);
        AscLogger::WriteStamped(2, "PlayerPollMgr::HandlePlayerPollSubmitResultOpcode: result=" + result
                                       + " pollID=" + std::to_string(pollId) + " opcode=" + std::to_string(opcode));
    }

    // Returns the question for a 1-based index, raising the original's Lua errors otherwise.
    Question* QuestionAt(lua_State* L, uint32_t index)
    {
        if (index == 0)
        {
            AscLua::luaL_error(L, "questionIndex must be greater than 0");
            return nullptr;
        }
        if (index - 1 >= g_questions.size())
        {
            AscLua::luaL_error(L, ("questionIndex " + std::to_string(index) + " is out of range").c_str());
            return nullptr;
        }
        return &g_questions[index - 1];
    }

    int ChoiceIndex(lua_State* L, const Question& q, uint32_t index)   // FUN_102d8700
    {
        if (index == 0)
            return AscLua::luaL_error(L, "choiceIndex must be greater than 0"), -1;
        if (index - 1 >= q.choices.size())
            return AscLua::luaL_error(L, ("choiceIndex " + std::to_string(index) + " is out of range").c_str()), -1;
        return static_cast<int>(index - 1);
    }

    // FUN_102d5db0: (number, number?, string?).
    struct SubmitArgs { uint32_t question = 0; bool hasChoice = false; uint32_t choice = 0; bool hasFeedback = false; std::string feedback; };
    bool ReadSubmitArgs(lua_State* L, SubmitArgs& a)
    {
        const int t2 = AscLua::lua_type(L, 2) > 0 ? NUMBER : NIL;
        const int t3 = AscLua::lua_type(L, 3) > 0 ? STRING : NIL;
        if (!ValidateInput(L, {NUMBER, t2, t3}))
            return false;
        a.question = static_cast<uint32_t>(static_cast<int64_t>(CheckNumber(L, 1)));
        if (AscLua::lua_type(L, 2) > 0)
        {
            a.hasChoice = true;
            a.choice = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        }
        if (AscLua::lua_type(L, 3) > 0)
        {
            a.hasFeedback = true;
            a.feedback = CheckString(L, 3);
        }
        return true;
    }

    // FUN_102d7b30
    bool CanSubmit(const Question& q, bool hasChoice, uint32_t choiceIndex, bool hasFeedback, const std::string& feedback)
    {
        if (q.Done() && !q.changeable)
            return false;
        if (hasFeedback && feedback.size() > kMaxFeedback)
            return false;
        uint32_t choiceId = 0;
        if (!hasChoice)
        {
            if (q.type != 4 || !hasFeedback || feedback.empty())
                return false;
        }
        else
        {
            if (choiceIndex >= q.choices.size())
                return false;
            choiceId = q.choices[choiceIndex].id;
        }
        const bool sameChoice = (q.answered && hasChoice) ? q.answer == choiceId : q.answered == hasChoice;
        if (!sameChoice)
            return true;
        const bool sameFeedback = (q.hasFeedback && hasFeedback) ? q.feedback == feedback : q.hasFeedback == hasFeedback;
        return !sameFeedback;
    }

    int CheckedSubmit(lua_State* L, bool send, const char* usage)
    {
        SubmitArgs a;
        if (!ReadSubmitArgs(L, a))
            return AscLua::luaL_error(L, usage);
        if (a.hasFeedback && a.feedback.size() > kMaxFeedback)
            return AscLua::luaL_error(L, ("feedbackText must not exceed " + std::to_string(kMaxFeedback) + " characters").c_str());
        Question* q = QuestionAt(L, a.question);
        bool hasChoice = false;
        uint32_t choiceIndex = 0;
        if (a.hasChoice && a.choice != 0)
        {
            choiceIndex = static_cast<uint32_t>(ChoiceIndex(L, *q, a.choice));
            hasChoice = true;
        }
        bool ok = CanSubmit(*q, hasChoice, choiceIndex, a.hasFeedback, a.feedback);
        if (ok && send)
        {
            Packet pkt(0x747);
            pkt.U32(q->id);
            pkt.U8(hasChoice ? 1 : 0);
            if (hasChoice)
                pkt.U32(q->choices[choiceIndex].id);
            pkt.U8(a.hasFeedback ? 1 : 0);
            if (a.hasFeedback)
                pkt.Str(a.feedback.c_str());
            pkt.Send();
        }
        PushBool(L, ok);
        return 1;
    }

    int CanSubmitAnswer(lua_State* L)
    {
        return CheckedSubmit(L, false, "Usage: C_PlayerPoll.CanSubmitAnswer(questionIndex, choiceIndex?, feedbackText?)");
    }
    int SubmitAnswer(lua_State* L)
    {
        return CheckedSubmit(L, true, "Usage: C_PlayerPoll.SubmitAnswer(questionIndex, choiceIndex?, feedbackText?)");
    }

    int CanChangeQuestionChoice(lua_State* L)
    {
        uint32_t index;
        if (!ReadNumber(L, index))
            return AscLua::luaL_error(L, "Usage: C_PlayerPoll.CanChangeQuestionChoice(questionIndex)");
        const Question* q = QuestionAt(L, index);
        PushBool(L, q && (q->changeable || !q->Done()));
        return 1;
    }

    int GetNumQuestionChoices(lua_State* L)
    {
        uint32_t index;
        if (!ReadNumber(L, index))
            return AscLua::luaL_error(L, "Usage: C_PlayerPoll.GetNumQuestionChoices(questionIndex)");
        const Question* q = QuestionAt(L, index);
        PushInt(L, static_cast<int32_t>(q->choices.size()));
        return 1;
    }

    int GetNumQuestions(lua_State* L)
    {
        if (AscLua::lua_gettop(L) != 0)
            return AscLua::luaL_error(L, "C_PlayerPoll.GetNumQuestions takes no arguments");
        PushInt(L, static_cast<int32_t>(g_questions.size()));
        return 1;
    }

    // FUN_102d8d80: (text, isSelected).
    int GetQuestionChoiceInfo(lua_State* L)
    {
        if (!ValidateInput(L, {NUMBER, NUMBER}))
            return AscLua::luaL_error(L, "Usage: C_PlayerPoll.GetQuestionChoiceInfo(questionIndex, choiceIndex)");
        const uint32_t qi = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        const uint32_t ci = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        const Question* q = QuestionAt(L, qi);
        const Choice& c = q->choices[static_cast<uint32_t>(ChoiceIndex(L, *q, ci))];
        PushStr(L, c.text.c_str());
        PushBool(L, q->answered && q->answer == c.id);
        return 2;
    }

    // FUN_102d8ff0: text, str20, str8C or nil, feedback or nil, str38.
    int GetQuestionInfo(lua_State* L)
    {
        uint32_t index;
        if (!ReadNumber(L, index))
            return AscLua::luaL_error(L, "Usage: C_PlayerPoll.GetQuestionInfo(questionIndex)");
        const Question* q = QuestionAt(L, index);
        PushStr(L, q->text.c_str());
        PushStr(L, q->str20.c_str());
        if (q->hasStr8C) PushStr(L, q->str8C.c_str()); else AscLua::lua_pushnil(L);
        if (q->hasFeedback) PushStr(L, q->feedback.c_str()); else AscLua::lua_pushnil(L);
        PushStr(L, q->str38.c_str());
        return 5;
    }

    // FUN_102d91e0: PLAYER_POLL_* by type; an out-of-range type logs and reads as index 1.
    int GetQuestionType(lua_State* L)
    {
        static const char* const kTypes[5] = {"NONE", "PLAYER_POLL_CHOOSE_ONE", "PLAYER_POLL_MULTIPLE_CHOICE",
            "PLAYER_POLL_RATING_SCALE", "PLAYER_POLL_OPTIONAL_CHOICE"};
        uint32_t index;
        if (!ReadNumber(L, index))
            return AscLua::luaL_error(L, "Usage: C_PlayerPoll.GetQuestionType(questionIndex)");
        const Question* q = QuestionAt(L, index);
        PushStr(L, kTypes[q->type <= 4 ? q->type : 1]);
        return 1;
    }

    int HasUnansweredQuestions(lua_State* L)
    {
        if (AscLua::lua_gettop(L) != 0)
            return AscLua::luaL_error(L, "C_PlayerPoll.HasUnansweredQuestions takes no arguments");
        PushBool(L, g_unanswered);
        return 1;
    }

    int RequestQuestionList(lua_State* L)
    {
        if (AscLua::lua_gettop(L) != 0)
            return AscLua::luaL_error(L, "C_PlayerPoll.RequestQuestionList takes no arguments");
        Packet(0x745).Send();
        PushBool(L, true);
        return 1;
    }

    // FUN_102d7ce0 (every glue screen): the questions dropped and the flag cleared.
    void Reset()
    {
        g_questions.clear();
        g_unanswered = false;
    }
    // FUN_102d8640 (after every world entry): CMSG 0x745, the question-list request, sent through the
    // static 0x406F40.
    void RequestAfterEnterWorld()
    {
        uint8_t store[0x18];
        reinterpret_cast<void(__thiscall*)(void*)>(0x401050)(store);
        reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(0x47B0A0)(store, 0x745);
        reinterpret_cast<void(__thiscall*)(void*)>(0x401130)(store);
        reinterpret_cast<void(__cdecl*)(void*)>(0x406F40)(store);
        reinterpret_cast<void(__thiscall*)(void*)>(0x403880)(store);
    }

    void Init()
    {
        AscRuntime::OnGlueScreen(&Reset);
        AscRuntime::OnAfterEnterWorld(&RequestAfterEnterWorld);
        sDC.AddPacketHandler(0x746, CNetClientCustomPacket((void*)&OnPollList, nullptr));
        sDC.AddPacketHandler(0x748, CNetClientCustomPacket((void*)&OnSubmitResult, nullptr));
    }

    const AscBindings::Binding kBindings[] = {
        {"C_PlayerPoll", "CanChangeQuestionChoice", CanChangeQuestionChoice},
        {"C_PlayerPoll", "CanSubmitAnswer", CanSubmitAnswer},
        {"C_PlayerPoll", "GetNumQuestionChoices", GetNumQuestionChoices},
        {"C_PlayerPoll", "GetNumQuestions", GetNumQuestions},
        {"C_PlayerPoll", "GetQuestionChoiceInfo", GetQuestionChoiceInfo},
        {"C_PlayerPoll", "GetQuestionInfo", GetQuestionInfo},
        {"C_PlayerPoll", "GetQuestionType", GetQuestionType},
        {"C_PlayerPoll", "HasUnansweredQuestions", HasUnansweredQuestions},
        {"C_PlayerPoll", "RequestQuestionList", RequestQuestionList},
        {"C_PlayerPoll", "SubmitAnswer", SubmitAnswer},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}
