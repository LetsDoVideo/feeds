// test_e2p_reader.cpp — deterministic coverage for the >4KB E2P reader fix
// (v1.5.1) without a live meeting.
//
// The engine→plugin (E2P) pipe is message-mode with a 4096-byte read buffer. A
// message larger than that is delivered as multiple ReadFile slices (the first
// N-1 with ERROR_MORE_DATA), which the reader must concatenate. Before v1.5.1
// that path killed the reader, silently freezing ALL engine→plugin traffic; the
// fix accumulates the slices. A solo dev can't convene a 50-participant webinar,
// but the trigger is byte count, not head count — a large roster JSON is unit-
// testable. This drives the SAME production code two ways:
//
//   1. AccumulateE2PMessageSlice (common/feeds-ipc-reassembly.h) — the exact
//      reassembly the reader uses — fed fabricated slices chopped at 4096 the way
//      a message-mode pipe delivers them.
//   2. A REAL Windows message-mode named pipe (same params as engine-client.cpp:
//      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE, 4096-byte buffers), so the OS
//      itself chops the message and the reader reassembles it end-to-end.
//
// Each reassembled message is then parsed with the SAME helpers the plugin's
// participant_list_changed handler uses (common/feeds-json-lite.h:
// SplitJsonObjects / JsonExtractArrayBody / ExtractJsonNumber / ExtractJsonString),
// asserting participant count, ids, and names survive intact — which doubles as a
// regression test for the string-aware object splitting from the same v1.5.1
// batch (display names containing '{', '}', '[', ']').
//
// Standalone console exe: depends only on the two shared headers + Win32. No
// libobs/Qt/SDK. Returns 0 on success, nonzero on any failure (ctest-friendly).

#include "feeds-ipc-reassembly.h"
#include "feeds-json-lite.h"

#include <windows.h>
#include <string>
#include <vector>
#include <thread>
#include <cstdio>
#include <cstdint>

using namespace feeds;

// ---------------------------------------------------------------------------
// Tiny assert harness (no framework dependency).
// ---------------------------------------------------------------------------
static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(cond)) {                                                          \
            std::printf("  FAIL: %s  [%s:%d]\n", (msg), __FILE__, __LINE__);    \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

// ---------------------------------------------------------------------------
// Roster message builder — mirrors engine-meeting.cpp SendParticipantList byte
// for byte: {"type":"participant_list_changed","my_user_id":M,"participants":[
//            {"id":U,"name":"<JsonEscape(name)>","muted":0|1}, ... ]}
// ---------------------------------------------------------------------------
struct Part {
    unsigned int id;
    std::string  name;
    bool         muted;
};

static std::string BuildRoster(unsigned int myId, const std::vector<Part>& ps) {
    std::string m = "{\"type\":\"participant_list_changed\",\"my_user_id\":";
    m += std::to_string(myId);
    m += ",\"participants\":[";
    bool first = true;
    for (const Part& p : ps) {
        if (!first) m += ",";
        first = false;
        m += "{\"id\":" + std::to_string(p.id) +
             ",\"name\":\"" + JsonEscape(p.name) + "\"" +
             ",\"muted\":" + (p.muted ? "1" : "0") + "}";
    }
    m += "]}";
    return m;
}

// Build a roster of `count` participants with names of `nameLen` chars, so tests
// can dial total byte size across the 4096 boundary.
static std::vector<Part> MakeParticipants(size_t count, size_t nameLen,
                                          unsigned int baseId = 16778240) {
    std::vector<Part> ps;
    ps.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        std::string name = "User" + std::to_string(i) + "_";
        while (name.size() < nameLen) name += 'x';
        name.resize(nameLen > name.size() ? name.size() : nameLen);
        ps.push_back({ baseId + (unsigned int)i, name, (i % 2) == 0 });
    }
    return ps;
}

// ---------------------------------------------------------------------------
// Parse a roster exactly as the plugin's participant_list_changed handler does.
// ---------------------------------------------------------------------------
static std::vector<Part> ParseRoster(const std::string& json, unsigned int& myId) {
    myId = (unsigned int)ExtractJsonNumber(json, "my_user_id");
    std::vector<Part> out;
    for (const std::string& obj :
         SplitJsonObjects(JsonExtractArrayBody(json, "participants"))) {
        Part p;
        p.id    = (unsigned int)ExtractJsonNumber(obj, "id");
        p.name  = ExtractJsonString(obj, "name");     // matches the handler
        p.muted = ExtractJsonNumber(obj, "muted") != 0;
        if (p.id != 0 && !p.name.empty()) out.push_back(p);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Reassembly driver #1 — fabricated slices, chopped at 4096 exactly the way a
// message-mode pipe delivers a large message. Returns the reassembled message
// (must equal the input) or reports if the reader would have produced none.
// ---------------------------------------------------------------------------
static bool ReassembleViaSlices(const std::string& full, std::string& out) {
    constexpr size_t BUF = 4096;
    std::string acc;
    bool produced = false;
    for (size_t off = 0; off < full.size() || off == 0; off += BUF) {
        size_t sliceLen = (full.size() - off) < BUF ? (full.size() - off) : BUF;
        bool moreData = (off + sliceLen) < full.size();
        std::string done;
        if (AccumulateE2PMessageSlice(acc, full.data() + off, sliceLen,
                                      moreData, done)) {
            out = std::move(done);
            produced = true;
        }
        if (!moreData) break;   // completing slice consumed
    }
    return produced;
}

// ---------------------------------------------------------------------------
// Reassembly driver #2 — a REAL message-mode named pipe loopback. Writes `full`
// from the "engine" end; reads + reassembles on the "plugin" end with the exact
// ReadFile + ERROR_MORE_DATA + AccumulateE2PMessageSlice loop from
// PipeReaderThread. Proves the OS chops at 4096 and the reader survives.
// ---------------------------------------------------------------------------
static bool ReassembleViaRealPipe(const std::string& full, std::string& out) {
    // Unique-ish name; PID keeps parallel ctest runs from colliding.
    std::string pipeName = "\\\\.\\pipe\\FeedsE2PTest_" +
                           std::to_string((unsigned long)GetCurrentProcessId());

    HANDLE server = CreateNamedPipeA(
        pipeName.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1, 4096, 4096, 0, NULL);              // 4096 buffers — matches production
    if (server == INVALID_HANDLE_VALUE) {
        std::printf("  ERROR: CreateNamedPipe failed: %lu\n", GetLastError());
        return false;
    }

    // Writer ("engine") thread: connect and write the whole message once.
    std::thread writer([&]() {
        HANDLE client = CreateFileA(pipeName.c_str(), GENERIC_WRITE, 0, NULL,
                                    OPEN_EXISTING, 0, NULL);
        if (client == INVALID_HANDLE_VALUE) return;
        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(client, &mode, NULL, NULL);
        DWORD written = 0;
        WriteFile(client, full.data(), (DWORD)full.size(), &written, NULL);
        FlushFileBuffers(client);
        CloseHandle(client);
    });

    ConnectNamedPipe(server, NULL);           // returns once the writer connects

    char buffer[4096];
    std::string acc;
    bool produced = false;
    for (;;) {
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(server, buffer, sizeof(buffer), &bytesRead, NULL);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_MORE_DATA) {
                std::string unused;
                AccumulateE2PMessageSlice(acc, buffer, bytesRead, true, unused);
                continue;
            }
            break;                            // BROKEN_PIPE once writer closes
        }
        std::string done;
        if (AccumulateE2PMessageSlice(acc, buffer, bytesRead, false, done)) {
            out = std::move(done);
            produced = true;
            break;                            // one message is enough for the test
        }
    }

    writer.join();
    CloseHandle(server);
    return produced;
}

// ---------------------------------------------------------------------------
// Shared assertions for a reassembled roster.
// ---------------------------------------------------------------------------
static void CheckRosterIntact(const std::string& sent, const std::string& got,
                              unsigned int expectMyId,
                              const std::vector<Part>& expect,
                              const char* label) {
    CHECK(got == sent, (std::string(label) + ": reassembled bytes match sent").c_str());
    CHECK(got.size() == sent.size(),
          (std::string(label) + ": no truncation (length equal)").c_str());

    unsigned int myId = 0;
    std::vector<Part> parsed = ParseRoster(got, myId);
    CHECK(myId == expectMyId, (std::string(label) + ": my_user_id parsed").c_str());
    CHECK(parsed.size() == expect.size(),
          (std::string(label) + ": participant count parsed").c_str());

    size_t n = parsed.size() < expect.size() ? parsed.size() : expect.size();
    bool idsOk = true, namesOk = true;
    for (size_t i = 0; i < n; ++i) {
        if (parsed[i].id != expect[i].id) idsOk = false;
        if (parsed[i].name != expect[i].name) namesOk = false;
    }
    CHECK(idsOk, (std::string(label) + ": all ids intact").c_str());
    CHECK(namesOk, (std::string(label) + ": all names intact").c_str());
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// Sizes straddling the 4096 boundary: just under, just over, several buffers.
static void test_size_matrix() {
    std::printf("[test_size_matrix]\n");
    struct Case { const char* label; std::vector<Part> ps; };

    // ~20 chars/name. Fixed per-object ~36 bytes → ~56 bytes/participant.
    std::vector<Case> cases = {
        { "under-4096  (~60 parts)",  MakeParticipants(60,  20) },
        { "over-4096   (~90 parts)",  MakeParticipants(90,  20) },
        { "4x-4096     (~300 parts)", MakeParticipants(300, 20) },
        { "10x-4096    (~700 parts)", MakeParticipants(700, 20) },
    };

    for (auto& c : cases) {
        std::string sent = BuildRoster(101, c.ps);
        std::printf("  %s -> %zu bytes\n", c.label, sent.size());

        std::string viaSlices;
        bool okS = ReassembleViaSlices(sent, viaSlices);
        CHECK(okS, (std::string(c.label) + ": slice reader produced a message").c_str());
        if (okS) CheckRosterIntact(sent, viaSlices, 101, c.ps,
                                   (std::string(c.label) + " [slices]").c_str());

        std::string viaPipe;
        bool okP = ReassembleViaRealPipe(sent, viaPipe);
        CHECK(okP, (std::string(c.label) + ": real-pipe reader produced a message").c_str());
        if (okP) CheckRosterIntact(sent, viaPipe, 101, c.ps,
                                   (std::string(c.label) + " [realpipe]").c_str());
    }
}

// Pinpoint the boundary: messages of length exactly 4095 / 4096 / 4097 / 8192.
static void test_exact_boundary() {
    std::printf("[test_exact_boundary]\n");
    for (size_t target : { (size_t)4095, (size_t)4096, (size_t)4097, (size_t)8192 }) {
        // One participant, pad its name so the whole message is exactly `target`.
        std::vector<Part> ps = { { 16778240u, "", false } };
        std::string probe = BuildRoster(7, ps);
        if (probe.size() > target) {
            std::printf("  (skip %zu: minimum message already %zu bytes)\n",
                        target, probe.size());
            continue;
        }
        size_t pad = target - probe.size();
        ps[0].name = std::string(pad, 'A');
        std::string sent = BuildRoster(7, ps);
        CHECK(sent.size() == target,
              ("exact size " + std::to_string(target)).c_str());

        std::string viaSlices, viaPipe;
        CHECK(ReassembleViaSlices(sent, viaSlices) && viaSlices == sent,
              ("boundary " + std::to_string(target) + " [slices] intact").c_str());
        CHECK(ReassembleViaRealPipe(sent, viaPipe) && viaPipe == sent,
              ("boundary " + std::to_string(target) + " [realpipe] intact").c_str());
    }
}

// Adversarial display names that look like JSON structure — the exact v1.5.1
// string-aware-splitting regression. Names carry '{', '}', '[', ']', ',', ':'.
// Combined with a payload that crosses 4096 so both fixes are exercised at once.
static void test_adversarial_names() {
    std::printf("[test_adversarial_names]\n");
    std::vector<Part> ps = {
        { 16778240u, "{TAG} Bob",              false },
        { 16778241u, "Bob :]",                 true  },
        { 16778242u, "[VIP] Alice {Producer}", false },
        { 16778243u, "a,b,c:d",                true  },
        { 16778244u, "}}}{{{ ][ ][",           false },
        { 16778245u, "normal name",            true  },
    };
    // Pad the roster past 4096 with filler participants so reassembly is also in
    // play while the adversarial names sit at the front and back.
    std::vector<Part> filler = MakeParticipants(90, 20, 20000000u);
    // Front adversarial, then filler, then one more adversarial at the tail.
    std::vector<Part> all = ps;
    all.insert(all.end(), filler.begin(), filler.end());
    all.push_back({ 16778246u, "tail {name] with :,",  false });

    std::string sent = BuildRoster(101, all);
    std::printf("  adversarial roster -> %zu bytes (%zu participants)\n",
                sent.size(), all.size());
    CHECK(sent.size() > 4096, "adversarial payload crosses 4096");

    for (int mode = 0; mode < 2; ++mode) {
        const char* tag = mode == 0 ? "slices" : "realpipe";
        std::string got;
        bool ok = mode == 0 ? ReassembleViaSlices(sent, got)
                            : ReassembleViaRealPipe(sent, got);
        CHECK(ok, (std::string("adversarial [") + tag + "] produced a message").c_str());
        if (!ok) continue;
        CHECK(got == sent, (std::string("adversarial [") + tag + "] bytes intact").c_str());

        unsigned int myId = 0;
        std::vector<Part> parsed = ParseRoster(got, myId);
        // The whole point of the string-aware splitter: braces/brackets inside a
        // name must NOT desync object splitting, so the count stays exact.
        CHECK(parsed.size() == all.size(),
              (std::string("adversarial [") + tag + "] count exact despite {}[] in names").c_str());
        // Spot-check the adversarial names round-trip (none contain a quote, so
        // ExtractJsonString reproduces them exactly — matching the handler).
        bool namesOk = parsed.size() == all.size();
        for (size_t i = 0; namesOk && i < all.size(); ++i)
            if (parsed[i].name != all[i].name) namesOk = false;
        CHECK(namesOk, (std::string("adversarial [") + tag + "] names round-trip").c_str());
    }
}

// A reader that survived a large message must keep working for the NEXT message —
// the pre-fix bug killed the reader thread, freezing all later traffic.
static void test_reader_survives_sequence() {
    std::printf("[test_reader_survives_sequence]\n");
    std::vector<std::string> msgs = {
        BuildRoster(1, MakeParticipants(200, 25)),   // big
        BuildRoster(1, MakeParticipants(2,   10)),   // small, must still parse
        BuildRoster(1, MakeParticipants(500, 30)),   // bigger
    };
    // Feed all three through ONE accumulator via slices, back to back, and make
    // sure each emerges whole and in order.
    std::string acc;
    std::vector<std::string> got;
    for (const std::string& full : msgs) {
        constexpr size_t BUF = 4096;
        for (size_t off = 0; off < full.size() || off == 0; off += BUF) {
            size_t sliceLen = (full.size() - off) < BUF ? (full.size() - off) : BUF;
            bool moreData = (off + sliceLen) < full.size();
            std::string done;
            if (AccumulateE2PMessageSlice(acc, full.data() + off, sliceLen,
                                          moreData, done))
                got.push_back(std::move(done));
            if (!moreData) break;
        }
    }
    CHECK(got.size() == msgs.size(), "all messages in a sequence emerged");
    bool ordered = got.size() == msgs.size();
    for (size_t i = 0; ordered && i < msgs.size(); ++i)
        if (got[i] != msgs[i]) ordered = false;
    CHECK(ordered, "sequenced messages intact and in order");
}

int main() {
    std::printf("=== E2P >4KB reader / reassembly tests ===\n");
    test_size_matrix();
    test_exact_boundary();
    test_adversarial_names();
    test_reader_survives_sequence();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("PASS\n");
        return 0;
    }
    std::printf("FAILED\n");
    return 1;
}
