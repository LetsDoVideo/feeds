// engine-client-stub.cpp — NON-WINDOWS stand-in for engine-client.cpp.
//
// On Windows, engine-client.cpp launches FeedsEngine.exe (the Zoom Meeting SDK
// host) and talks to it over two named pipes. There is no engine on macOS yet:
// the Mac port of the engine is the next phase. This file provides the same
// four entry points so the plugin builds, loads, and shows its UI, with every
// engine-dependent action inert:
//
//   StartEngine            logs once that the engine is unavailable; false
//   StopEngine             no-op
//   SendToEngine           drops the message; false (DEBUG line, no payload)
//   RegisterMessageHandler accepted and ignored (no engine ever sends one)
//
// The plugin already tolerates an engine that never connects (the same state
// as a Windows engine that failed to launch): no login, no meeting, no
// participant video. CMake compiles this file instead of engine-client.cpp on
// every non-Windows platform.

#include <functional>
#include <string>

#include <obs-module.h>

namespace feeds {

bool StartEngine()
{
    blog(LOG_INFO,
         "[feeds] StartEngine: the Feeds engine is not available on this "
         "platform yet (macOS plugin shell). Zoom login, meetings and "
         "participant video are disabled.");
    return false;
}

void StopEngine() {}

bool SendToEngine(const std::string& /*jsonMessage*/)
{
    // Nothing of the payload is logged: join_meeting carries credentials.
    blog(LOG_DEBUG, "[feeds] SendToEngine: no engine on this platform; "
                    "message dropped");
    return false;
}

void RegisterMessageHandler(const std::string& /*messageType*/,
                            std::function<void(const std::string&)> /*handler*/)
{
}

}  // namespace feeds
