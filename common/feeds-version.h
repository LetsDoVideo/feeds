// feeds-version.h — Single source of truth for the Feeds version string.
//
// Both feeds.dll (plugin) and FeedsEngine.exe ship from the same commit
// and must agree on this value. The engine echoes it back to the plugin
// in the engine_ready handshake, so a mismatch would indicate a stale
// binary on one side.
//
// Bumping a release: change the literal here only. Display-level version
// strings in installer scripts and README are intentionally separate —
// they don't gate the engine/plugin handshake.

#pragma once

namespace feeds_shared {

static constexpr const char* VERSION = "1.3.0";

} // namespace feeds_shared
