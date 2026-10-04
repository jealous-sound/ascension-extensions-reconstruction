#pragma once
// The original Extensions.dll's module runtime: custom UI events, client timers and the lifecycle
// callback lists every subsystem registers into. Reconstructed from its init code, so a subsystem
// transcribed from the decompile can be wired exactly the way the original wires it.
//
//   Events     FUN_102783b0 registers a name; FUN_10278c90 looks it up in the client's event table
//              (0xD3F7D0: +4 count, +8 entries, entry +0 = hash 0x76F640, +0x14 = name);
//              FUN_100d0a50 fires it through FrameScript_SignalEvent (0x81B530) with typed varargs.
//   Timers     0x403370(ms, cb, param) schedules a one-shot; 0x4033B0(handle, cb, param) cancels.
//   Lifecycle  FUN_10278510 -> list 0x10be298c, run by the detour on 0x528010 BEFORE the original
//              (world UI entry; the original sets the in-world flag 0xBD0792).
//              FUN_10278760 -> list 0x10be286c, run from a cave over 0x4DA5F0's epilogue at 0x4DA9AC
//              (glue screen initialised -- i.e. back at login / character select).
#include <cstdint>
struct lua_State;

namespace AscRuntime
{
    // Appends every event name the original registers to the client's event table. Must run
    // before the world event table is built (the scaffold's RegisterEventEx at 0x52AB26).
    void RegisterEventNames();

    int EventId(const char* name);      // -1 if the client does not know it

    // The custom event names in registration order (the original's list 0x10BE2E58).
    const char* const* CustomEventNames(uint32_t& count);

    template <class... Args>
    void Signal(const char* name, const char* fmt, Args... args)
    {
        const int id = EventId(name);
        if (id >= 0)
            reinterpret_cast<int(__cdecl*)(int, const char*, ...)>(0x81B530)(id, fmt, args...);
    }

    // No arguments: the original leaves fmt = NULL on the stack (FUN_100b94b0 / the inline form).
    inline void Signal(const char* name)
    {
        const int id = EventId(name);
        if (id >= 0)
            reinterpret_cast<int(__cdecl*)(int, const char*)>(0x81B530)(id, nullptr);
    }

    typedef int(__cdecl* TimerFn)(void* param);
    uint32_t Schedule(uint32_t ms, TimerFn fn, void* param);
    void Cancel(uint32_t handle, TimerFn fn, void* param);

    typedef void (*Callback)();
    void OnEnterWorld(Callback cb);     // FUN_10278510
    void OnGlueScreen(Callback cb);     // FUN_10278760
    void OnAfterEnterWorld(Callback cb); // FUN_10278780 -> list 0x10be29ac, after 0x528010 returns

    // FUN_10278530: callbacks run when the client dispatches stock event `eventId` (detour on the
    // per-event dispatcher 0x81AA00(eventId, L, nargs), callbacks before the original).
    typedef void (*StockEventCallback)(lua_State* L);
    void OnStockEvent(int eventId, StockEventCallback cb);
    const char* EventName(int eventId);   // client event table entry +0x14, or nullptr

    // FUN_102785c0 -> list 0x10be28ac, run by the detour 0x10275CA0 (hook object 0x10BCB52C) BEFORE
    // 0x528C30, the world UI teardown (it signals an event, then unloads the world subsystems).
    void OnLeaveWorld(Callback cb);
    // FUN_10278640 -> list 0x10be28cc, run by the detour 0x10275D40 (hook object 0x10BCB538) before
    // 0x528F00. FUN_10278370 -> list 0x10be2a8c, run by the detour 0x10277FB0 (hook object 0x10BCB550)
    // after 0x402910 returns.
    void OnBefore528F00(Callback cb);
    void OnAfter402910(Callback cb);
    // FUN_10278740 -> list 0x10BE2DB8: run with the object after client 0x6E7F50 (__thiscall(object))
    // returns (the original's detour 0x10275E90, hook object 0x10BCB65C).
    void OnAfter6E7F50(void (*cb)(void* object));
    // FUN_10278560 -> list 0x10BE2B0C: run with the first argument before client 0x4943C0 (__cdecl(a, b),
    // a key event), only while the byte 0xBD0792 is set (the original's detour 0x102763F0).
    void OnBefore4943C0(void (*cb)(const uint32_t* event));
    // FUN_10278450 -> list 0x10BE2AEC: run with the spell after the spellbook add 0x542030 (AscWildcardRolls
    // owns that detour, FUN_102781e0, and calls RunAfterAddSpell).
    void OnAfterAddSpell(void (*cb)(uint32_t spell));
    void RunAfterAddSpell(uint32_t spell);

    // FUN_10278720 -> list 0x10be290c: run with the first argument after the client function 0x5204C0
    // (__cdecl, seven arguments) returns (the original's detour 0x10275CD0, hook object 0x10BCB568).
    void OnAfter5204C0(void (*cb)(void* first));
    // FUN_10278700 -> list 0x10be2aac: run after 0x4F6F90 (__thiscall, no stack arguments; the world
    // scene render) returns (the original's detour 0x10276270, hook object 0x10BCB5A4).
    void OnAfter4F6F90(Callback cb);

    // FUN_10278680: run after the client function 0x403340 (list 0x10be29cc; the original's detour
    // 0x10276680). Used for per-player refresh checks such as the tutorial level watch.
    void OnAfter403340(Callback cb);

    // FUN_102786a0 -> list 0x10be288c: run after the client function 0x495810 (a, b, c, float dt) returns
    // (the original's detour 0x102776F0, hook object 0x10BCB520).
    void OnAfter495810(Callback cb);

    // FUN_102787a0 -> list 0x10be2bec: run with the object before client 0x6FC0F0 removes it.
    typedef void (*ObjectCallback)(void* object);
    void OnObjectFree(ObjectCallback cb);
    // FUN_102785e0 -> list 0x10be28ec: run on every second call of the stock packet handler 0x6DF050.
    void OnAlternate6DF050(Callback cb);
    // The original's hook-manager detours over a game object's use (0x70F680, detour 0x10A42DD0) and
    // an item's use (0x708C20, detour 0x10A42F30): the original runs first, then these, with the object.
    void OnGameObjectUse(ObjectCallback cb);
    void OnItemUse(ObjectCallback cb);
    // FUN_10278470 -> list 0x10be2cf8: the original's detour 0x10275E50 over 0x743EC0 (__thiscall(object,
    // a), ret 4; hook object 0x10BCB5B0) runs the original, then these with the object.
    void OnAfter743EC0(ObjectCallback cb);
    // The original's detour 0x10275D70 over the target change 0x524BF0: FUN_10278390 -> list 0x10BE2950
    // before the original, FUN_10278330 -> list 0x10BE2970 after it (not while 0xBD0791 is set).
    void OnBeforeTargetChange(Callback cb);
    // FUN_10278600 -> list 0x10BE29EC: after client 0x52A980, only when the byte 0xBD0790 was set before it
    // (the original's detour 0x10275C60).
    void OnAfter52A980(Callback cb);
    void OnAfterTargetChange(Callback cb);
    // FUN_10278660 -> list 0x10BE2BAC: run with the object's +0x144 before client 0x6ECF80 (detour 0x102762B0).
    void OnBefore6ECF80(void (*cb)(uint32_t));
    // Veto lists over three spell/aura paths. Each callback returns false to stop: the rest are skipped
    // and so is (or, for 0x80B5D0, the result becomes false instead of) the client function.
    //   FUN_10278430 -> list 0x10BE2B6C, the detour 0x10275F60 over 0x724820 (__thiscall(unit, a, b), ret 8),
    //                   callbacks (unit, a, b) BEFORE the original;
    //   FUN_10278620 -> list 0x10BE2B8C, the detour 0x10276090 over 0x71E930 (same shape);
    //   FUN_102783D0 -> list 0x10BE2DD8, the detour 0x10277900 over 0x80B5D0 (__cdecl, 5 arguments,
    //                   returns bool): the original first; only when it is true, callbacks (a..e).
    typedef bool (__cdecl* UnitSpellVeto)(void* unit, uint32_t a, uint32_t b);
    // `site` identifies the original registration call. The aura-removal and cast lists use the measured
    // native unordered_set traversal. Other lists retain call-site ordering until fully measured.
    void OnBefore724820(UnitSpellVeto cb, uint32_t site);
    void OnBefore71E930(UnitSpellVeto cb, uint32_t site);
    // FUN_10278800 -> list 0x10BE2D78: (unit, spell effect object) -> true removes the effect (0x6F87C0).
    // Run over each unit's effect chain (+0xA8, next +0x108) by the replacement 0x10276310 of 0x744AC0 when
    // its argument is 0, and by the sweep 0x10277970 over every unit (after 0x403340).
    typedef bool (__cdecl* EffectFilter)(void* unit, void* effect);
    void OnEffectFilter(EffectFilter cb, uint32_t site);
    // FUN_10278820 -> list 0x10BE2D98, asked by FUN_10277870: true hides the spell's visual for the object
    // (guid kind, guid, spell).
    typedef bool (__cdecl* VisualHide)(uint32_t kind, uint32_t guidLo, uint32_t guidHi, uint32_t spell);
    void OnVisualHide(VisualHide cb, uint32_t site);
    bool VisualHidden(uint32_t kind, uint32_t guidLo, uint32_t guidHi, uint32_t spell);   // FUN_10277870
    typedef bool (__cdecl* CastVeto)(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
    void OnAfter80B5D0(CastVeto cb, uint32_t site);

    // The DLL's hook manager (FUN_100010f0) over a stock packet handler (a, opcode, time, packet): the
    // hook runs in its place and calls `original` itself. One hook per handler; 0x6DF050 composes with
    // OnAlternate6DF050. Call from a module init (before InstallHooks).
    typedef int(__cdecl* PacketHandler)(void* a, uint32_t opcode, uint32_t time, void* packet);
    typedef int(__cdecl* PacketHook)(PacketHandler original, void* a, uint32_t opcode, uint32_t time, void* packet);
    void HookPacketHandler(uint32_t address, uint32_t prologue, PacketHook hook);

    void InstallHooks();                // call once at attach

    // Overwrite a client function's entry with a jmp to `fn` -- for the original's detours that
    // never call their trampoline (e.g. FUN_10a4f500 over SMSG_SERVERTIME's 0x7E2A50).
    void ReplaceFunction(uint32_t target, void* fn);

    // The hook manager's plain detour (FUN_100010f0): `prologue` whole instructions of `target` are
    // relocated into a trampoline (returned, to call the original) and replaced by a jmp to `fn`.
    void* Detour(uint32_t target, uint32_t prologue, void* fn);
}
