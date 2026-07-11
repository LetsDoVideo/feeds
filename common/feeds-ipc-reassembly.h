// feeds-ipc-reassembly.h — E2P (engine→plugin) message reassembly.
//
// The E2P pipe is opened in BYTE/MESSAGE mode with a 4096-byte buffer (see
// engine-client.cpp CreateNamedPipe). A single logical message larger than the
// buffer is delivered by ReadFile as N slices: the first N-1 reads return FALSE
// with GetLastError() == ERROR_MORE_DATA (each filling the buffer with the next
// slice), and the final slice arrives on a read that returns TRUE. Those partial
// reads must be concatenated, not treated as fatal — otherwise a large roster
// (50+ participant webinar) or a large Zoom Events payload would kill the reader
// and silently stop ALL engine→plugin traffic for the session.
//
// The accumulation itself has no OS or libobs dependency, so it lives here as a
// pure function: engine-client.cpp's PipeReaderThread drives it from real
// ReadFile results, and the unit test drives it from fabricated slices (and from
// a real named-pipe loopback) — both exercise the same reassembly code.
#pragma once

#include <string>
#include <utility>

namespace feeds {

// Feed one pipe read into the message accumulator.
//   acc      — caller-owned accumulator; persists across calls, holds the bytes
//              seen so far for the in-flight message. Reset to empty on completion.
//   buf, n   — the slice this read produced (n bytes at buf).
//   moreData — true iff this read reported ERROR_MORE_DATA, i.e. the message is
//              incomplete and more slices follow. false for a completing read.
//   out      — receives the finished message (moved out of acc) when this call
//              completes one; left untouched otherwise.
// Returns true iff a complete, non-empty message was moved into `out`. A
// completing read that leaves the accumulator empty (n == 0 and nothing buffered)
// returns false, matching the reader's "skip empty" behavior.
inline bool AccumulateE2PMessageSlice(std::string& acc, const char* buf, size_t n,
                                      bool moreData, std::string& out) {
    if (n > 0) acc.append(buf, n);
    if (moreData) return false;      // partial — wait for the completing read
    if (acc.empty()) return false;   // completing read but nothing accumulated
    out = std::move(acc);
    acc.clear();
    return true;
}

} // namespace feeds
