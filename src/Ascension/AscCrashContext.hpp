#pragma once
// The "Ascension Debug" section of the client's crash report (AscCrashContext.cpp, installer FUN_1019dc80).
// Hooks across the client record the last thing each subsystem touched; the crash-text builder hook
// (0x682200) appends them. Some of the recorders belong to other installers -- they write through here.
#include <cstdint>
#include <string>

namespace AscCrashContext
{
    // FUN_10112f70: records `message` as the report's "Assert Info" (0x10BCA210), then raises the client's
    // fatal error 0x8C51D0(message, file, line).
    void Assert(const char* message, const char* file, int line);

    // Recorded by hooks in other installers. g_m2Cache, g_m2SetVertices, g_m2ShadowMap and g_asyncTexture
    // have no writer in the original either (their only references are the crash-text builder FUN_1019e5f0
    // and their atexit destructors), so they are always empty in its reports too.
    extern std::string g_signalEvent;      // 0x10BCBCC8  "Last FrameScript_SignalEvent"  (AscRuntime, FUN_102766c0)
    extern std::string g_m2Cache;          // 0x10BCA120  "Last Loaded M2 Name (CM2Cache)"
    extern std::string g_m2Shared;         // 0x10BCBCB0  "Last Loaded M2 Name (CM2Shared)"  (AscItemCollections, FUN_102cb000)
    extern std::string g_m2SetVertices;    // 0x10BCA3B4  "Last M2 SetVerticies Name"
    extern std::string g_m2ShadowMap;      // 0x10BCA240  "Last M2 Shadow Map Name"
    extern std::string g_asyncTexture;     // 0x10BC9018  "Last Async Texture"
    extern std::string g_textureCreate;    // 0x10BCB94C  "Last TextureCreate"
    extern uint32_t g_gameObjectEntry;     // 0x10BDED34  "Last GameObject::CGGameObject_C__ModelLoaded Entry"
    extern std::string g_gameObjectModel;  // 0x10BCA104  "... Model Name"
}
