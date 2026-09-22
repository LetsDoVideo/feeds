// feeds-iso-recorder.cpp — records one OBS source to its own ISO file (Feeds
// uses one per enrolled participant; see feeds-iso-recorder.h). See also
// C:\Dev\iso-recording-investigation.md for the original design rationale.
//
// This adopts Exeldro's Source Record patterns (C:\Dev\obs-source-record\
// source-record.c) — a private obs_view for video, a private audio_output
// for audio, and an obs_output muxer — used DIRECTLY inside the participant
// source rather than as an OBS filter, with the six known Source Record bugs
// fixed:
//
//   Bug 1: use-after-release in Source Record's audio callback. Our audio
//          callback never touches the OBS source at all: it only reads this
//          recorder's own timeline buffer, under audio_m, and the
//          audio_output is closed (its thread joined) before that buffer or
//          the recorder is freed.
//   Bug 2: unprotected global filter DARRAY. Our recorder registry is
//          mutex-protected.
//   Bug 3: encoder release racing the output release. Our teardown runs the
//          release chain in strict order from a single queued task.
//   Bug 4: output not draining before release (ISO files end short). We
//          call obs_output_stop (graceful) and release only after the
//          output's "stop" signal fires.
//   Bug 5: OBS hangs on close (busy-poll of obs_encoder_active). We use
//          deterministic signal-driven teardown with a bounded timeout that
//          force-stops once, never a busy-loop.
//   Bug 6: trigger mode stuck. We reset last_frontend_event when the
//          checkbox is enabled and start immediately if OBS is already
//          recording.
//
// Audio (Windows, FEEDS_ISO_ISOLATED_AUDIO): each file carries only its
// participant's isolated voice. The engine delivers that participant's Zoom
// one-way audio stamped with its arrival time on the QPC clock, which on
// Windows is the clock os_gettime_ns() reads, so audio and video timestamps
// share one timeline. push_audio places each chunk on a per-recording
// timeline buffer by that timestamp; the private audio_output's callback
// serves that buffer a fixed ISO_AUDIO_LATENCY_NS behind real time (so
// late-arriving chunks have landed) and reports the true timestamp via
// new_ts; obs_output then interleaves A/V by timestamp exactly as it does
// for OBS's own mix. Gaps (silence, a muted participant: Zoom sends nothing)
// are simply never written and read out as silence. There is deliberately
// no mixing of any kind: one participant, passed through.
// macOS keeps the program mix (obs_get_audio()) until its engine delivers
// participant audio; see FEEDS_ISO_ISOLATED_AUDIO.
//
// Threading: source lifecycle + frontend events arrive on the OBS UI thread;
// tick runs on the graphics thread. Both mutate the recorder, so the state
// machine is guarded by feeds_iso_recorder::lock. The isolated-audio state
// (timeline buffer, resampler, positions) is shared between push_audio (the
// source's pump thread) and the audio_output callback (a libobs audio
// thread), and is guarded by feeds_iso_recorder::audio_m instead; neither of
// those threads ever takes lock. The output's "stop" signal fires on an
// internal OBS thread and only sets an atomic flag + notifies a CV; the actual
// release happens on a core thread (tick, or destroy after a bounded wait).

#include "feeds-iso-recorder.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/platform.h>
#include <util/util_uint64.h>
#include <media-io/audio-io.h>
#include <media-io/audio-resampler.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace feeds {

namespace {

// Graceful-stop drain timeout. If the output's "stop" signal hasn't fired
// within this window we force-stop once and proceed, accepting a few lost
// trailing frames rather than hanging OBS shutdown (Bug 4 / Bug 5).
constexpr int TEARDOWN_TIMEOUT_MS = 5000;

// Ultimate filename fallback when no usable name is available.
constexpr const char *NAME_FALLBACK = "Feeds ISO";

// Audio bitrate (kbps) for the AAC track: the participant's isolated voice
// (Windows) or the program mix (macOS).
constexpr int AUDIO_BITRATE_KBPS = 160;

// Isolated-audio timing (see the file header).
//   LATENCY: how far behind real time the audio_output serves the timeline.
//            Chunks arrive within a few tens of ms of their stamp; this only
//            has to cover that, and it costs nothing in sync because packets
//            carry their true timestamps.
//   SMOOTH:  a chunk whose stamp is within this of where the previous chunk
//            ended continues it seamlessly (absorbs arrival jitter). Further
//            ahead = a gap, left silent. Further behind = we're running ahead
//            of the clock (burst or drift), so the chunk is dropped to let
//            the stamps catch up. The timeline never rewinds.
//   BUFFER:  timeline buffer length; must exceed LATENCY plus any plausible
//            arrival skew.
constexpr uint64_t ISO_AUDIO_LATENCY_NS = 150000000ULL;   // 150 ms
constexpr uint64_t ISO_AUDIO_SMOOTH_NS  = 50000000ULL;    // 50 ms
constexpr uint64_t ISO_AUDIO_BUFFER_NS  = 2000000000ULL;  // >= 2 s

}  // namespace

// ---------------------------------------------------------------------------
// Recorder state
// ---------------------------------------------------------------------------
struct feeds_iso_recorder {
	obs_source_t *parent = nullptr;  // borrowed; stable for the recorder's life
	feeds_iso_name_fn name_fn = nullptr;
	feeds_iso_started_fn started_fn = nullptr;
	void *name_ud = nullptr;

	// Fixed recording size (set_fixed_size); 0 x 0 = follow the parent's size.
	// Written before recording starts, read by tick (both under lock).
	uint32_t fixed_width = 0;
	uint32_t fixed_height = 0;

	// Guards the mutable state below against the UI thread (lifecycle +
	// frontend events) racing the graphics thread (tick). The audio
	// callback does NOT take this lock — see the file header.
	std::mutex lock;

	// Private render view + its video pipeline. Persist across recordings;
	// recreated when the parent's dimensions change (only while not draining,
	// so a draining output never has its mix freed underneath it).
	obs_view_t *view = nullptr;
	video_t *video_output = nullptr;
	uint32_t width = 0;
	uint32_t height = 0;

	// Per-recording output + encoders, created at start. On stop the output
	// is told to drain gracefully (Bug 4) and released — by the next tick once
	// the "stop" signal fires, or inline (bounded) by destroy. Only one output
	// drains at a time; new starts wait for the drain to finish.
	obs_output_t *output = nullptr;
	obs_encoder_t *venc = nullptr;
	obs_encoder_t *aenc = nullptr;
	bool output_active = false;  // obs_output_start succeeded, recording live
	bool draining = false;       // output told to stop, awaiting "stop" + release
	bool drain_forced = false;   // already force-stopped this drain (Bug 5)
	uint64_t drain_deadline_ns = 0;
	std::atomic<bool> drain_done{false};  // set by the "stop" signal handler
	std::mutex drain_m;                   // guards destroy's bounded drain wait
	std::condition_variable drain_cv;

	bool enabled = false;       // set by the owner (the ISO registry)
	bool want_record = false;   // OBS main recording active and we should record
	bool paused = false;        // desired pause state, mirrors main recording
	std::atomic<bool> closing{false};  // teardown in progress (short-circuits tick)
	int tier = 0;
	int last_frontend_event = -1;  // -1 == "none" sentinel (Bug 6)

	// --- Isolated participant audio (FEEDS_ISO_ISOLATED_AUDIO) --------------
	// Everything below is guarded by audio_m (push_audio on the pump thread vs
	// the audio_output callback), except audio_name, which start_output sets
	// before the pipeline opens and which is only read (for log lines) while it
	// is open. audio is non-null exactly while a recording's audio pipeline is
	// open; push_audio drops chunks when it is null.
	//
	// The timeline: sample position p (mono, audio_rate) is the instant
	// audio_base_ts + p / audio_rate. audio_ring holds positions modulo its
	// (power-of-two) size; unwritten positions are zero, and the callback
	// zeroes each position after emitting it.
	std::mutex audio_m;
	audio_t *audio = nullptr;
	std::string audio_name;
	uint32_t audio_rate = 0;
	uint64_t audio_base_ts = 0;
	std::vector<float> audio_ring;
	int64_t audio_next_pos = -1;  // where the next contiguous chunk lands; -1 = no anchor
	int64_t audio_emit_pos = -1;  // next position the callback emits; -1 = not started
	audio_resampler_t *resampler = nullptr;  // SDK format -> audio_rate float mono
	uint32_t rs_rate = 0;
	uint32_t rs_channels = 0;
	bool audio_logged_first = false;
	uint64_t audio_received_ns = 0;  // stats, logged when the recording closes
	uint64_t audio_late_frames = 0;
	uint64_t audio_ahead_frames = 0;
};

// ---------------------------------------------------------------------------
// Registry — every live recorder, so the single frontend-event callback can
// fan out to all of them. Mutex-protected (Bug 2: Source Record's equivalent
// DARRAY was unprotected).
// ---------------------------------------------------------------------------
static std::mutex g_registry_mutex;
static std::vector<feeds_iso_recorder *> g_registry;

// ---------------------------------------------------------------------------
// Encoder-ID resolution. Advanced mode stores canonical encoder IDs; Simple
// mode stores short names that must be translated. We port OBS 31's own
// get_simple_output_encoder() (frontend/utility/SimpleOutput.cpp:88-123)
// VERBATIM rather than the build brief's table, which was stale for QSV
// (obs_qsv11_v2, not obs_qsv11_h264) and NVENC (obs_nvenc_h264_tex preferred,
// not jim_nvenc). HEVC mappings are included unconditionally; if an encoder
// isn't available the start path falls back to obs_x264.
// ---------------------------------------------------------------------------
static bool encoder_available(const char *id)
{
	const char *val;
	for (int i = 0; obs_enum_encoder_types(i, &val); i++) {
		if (strcmp(val, id) == 0)
			return true;
	}
	return false;
}

static const char *simple_to_canonical_encoder(const char *enc)
{
	if (!enc || strcmp(enc, "x264") == 0 || strcmp(enc, "x264_lowcpu") == 0)
		return "obs_x264";
	if (strcmp(enc, "qsv") == 0)
		return "obs_qsv11_v2";
	if (strcmp(enc, "qsv_av1") == 0)
		return "obs_qsv11_av1";
	if (strcmp(enc, "amd") == 0)
		return "h264_texture_amf";
	if (strcmp(enc, "amd_hevc") == 0)
		return "h265_texture_amf";
	if (strcmp(enc, "amd_av1") == 0)
		return "av1_texture_amf";
	if (strcmp(enc, "nvenc") == 0)
		return encoder_available("obs_nvenc_h264_tex") ? "obs_nvenc_h264_tex" : "ffmpeg_nvenc";
	if (strcmp(enc, "nvenc_hevc") == 0)
		return encoder_available("obs_nvenc_hevc_tex") ? "obs_nvenc_hevc_tex" : "ffmpeg_hevc_nvenc";
	if (strcmp(enc, "nvenc_av1") == 0)
		return "obs_nvenc_av1_tex";
	if (strcmp(enc, "apple_h264") == 0)
		return "com.apple.videotoolbox.videoencoder.ave.avc";
	if (strcmp(enc, "apple_hevc") == 0)
		return "com.apple.videotoolbox.videoencoder.ave.hevc";
	return "obs_x264";
}

// Map an OBS recording format (RecFormat2) to a file extension. Mirrors
// Source Record's GetFormatExt (source-record.c:238-253).
static const char *format_extension(const char *format)
{
	if (!format || !*format)
		return "mp4";
	if (strcmp(format, "fragmented_mp4") == 0 || strcmp(format, "hybrid_mp4") == 0)
		return "mp4";
	if (strcmp(format, "fragmented_mov") == 0 || strcmp(format, "hybrid_mov") == 0)
		return "mov";
	if (strcmp(format, "hls") == 0)
		return "m3u8";
	if (strcmp(format, "mpegts") == 0)
		return "ts";
	return format;  // mp4, mkv, mov, flv, ts ...
}

// Map a format to the muxer output type. hybrid_* use the newer mp4/mov
// outputs; everything else uses ffmpeg_muxer
// (AdvancedOutput.cpp:109-113, SimpleOutput.cpp:236-240).
static const char *format_output_id(const char *format)
{
	if (format && strcmp(format, "hybrid_mp4") == 0)
		return "mp4_output";
	if (format && strcmp(format, "hybrid_mov") == 0)
		return "mov_output";
	return "ffmpeg_muxer";
}

// ---------------------------------------------------------------------------
// Filename: sanitise + build "<name> <YYYY-MM-DD HH-mm-ss>.<ext>".
// ---------------------------------------------------------------------------

// Decode one UTF-8 codepoint at s[i], advancing i past it. Returns the
// codepoint, or 0xFFFD on malformed input (advancing one byte).
static uint32_t utf8_next(const std::string &s, size_t &i)
{
	unsigned char c = static_cast<unsigned char>(s[i]);
	if (c < 0x80) {
		i += 1;
		return c;
	}
	int extra;
	uint32_t cp;
	if ((c & 0xE0) == 0xC0) {
		extra = 1;
		cp = c & 0x1F;
	} else if ((c & 0xF0) == 0xE0) {
		extra = 2;
		cp = c & 0x0F;
	} else if ((c & 0xF8) == 0xF0) {
		extra = 3;
		cp = c & 0x07;
	} else {
		i += 1;
		return 0xFFFD;
	}
	for (int k = 1; k <= extra; k++) {
		if (i + k >= s.size() || (static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
			i += 1;
			return 0xFFFD;
		}
		cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
	}
	i += extra + 1;
	return cp;
}

static void utf8_append(std::string &out, uint32_t cp)
{
	if (cp < 0x80) {
		out += static_cast<char>(cp);
	} else if (cp < 0x800) {
		out += static_cast<char>(0xC0 | (cp >> 6));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		out += static_cast<char>(0xE0 | (cp >> 12));
		out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	} else {
		out += static_cast<char>(0xF0 | (cp >> 18));
		out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
		out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	}
}

// True for codepoints we drop from filenames: control chars, zero-width /
// joiners, variation selectors, and a pragmatic emoji range set. Accented
// Latin and CJK fall through and are kept. Emoji detection is range-based and
// approximate by design (the goal is "no emoji in filenames", not Unicode
// perfection).
static bool is_droppable_codepoint(uint32_t cp)
{
	if (cp < 0x20 || cp == 0x7F)               // C0 controls + DEL
		return true;
	if (cp >= 0x80 && cp <= 0x9F)              // C1 controls
		return true;
	if (cp >= 0x200B && cp <= 0x200F)          // zero-width space..RLM
		return true;
	if (cp == 0x2028 || cp == 0x2029)          // line/paragraph separators
		return true;
	if (cp == 0x2060 || cp == 0xFEFF)          // word joiner / BOM-ZWNBSP
		return true;
	if (cp >= 0xFE00 && cp <= 0xFE0F)          // variation selectors
		return true;
	if (cp >= 0x2190 && cp <= 0x21FF)          // arrows (often emoji-styled)
		return true;
	if (cp >= 0x2300 && cp <= 0x27BF)          // misc technical .. dingbats
		return true;
	if (cp >= 0x2B00 && cp <= 0x2BFF)          // misc symbols & arrows
		return true;
	if (cp >= 0x1F000 && cp <= 0x1FAFF)        // SMP emoji blocks
		return true;
	if (cp >= 0x1F1E6 && cp <= 0x1F1FF)        // regional indicators (flags)
		return true;
	return false;
}

// Apply the filename rules: replace Windows-reserved chars with '_', drop
// droppable codepoints, keep everything else (accented Latin, CJK), then trim
// leading/trailing whitespace and dots. Returns NAME_FALLBACK if empty.
static std::string sanitize_filename(const std::string &in)
{
	std::string out;
	out.reserve(in.size());
	size_t i = 0;
	while (i < in.size()) {
		uint32_t cp = utf8_next(in, i);
		switch (cp) {
		case '/':
		case '\\':
		case ':':
		case '*':
		case '?':
		case '"':
		case '<':
		case '>':
		case '|':
			out += '_';
			continue;
		default:
			break;
		}
		if (is_droppable_codepoint(cp))
			continue;
		utf8_append(out, cp);
	}

	// Trim leading/trailing whitespace and dots (Windows trims trailing
	// dots/spaces silently, a known source of weird behaviour).
	auto is_trim = [](char c) { return c == ' ' || c == '\t' || c == '.'; };
	size_t b = 0, e = out.size();
	while (b < e && is_trim(out[b]))
		b++;
	while (e > b && is_trim(out[e - 1]))
		e--;
	out = out.substr(b, e - b);

	if (out.empty())
		return NAME_FALLBACK;
	return out;
}

// Resolve the name (once, at start) via the supplied hook, falling back to
// the OBS source name and finally NAME_FALLBACK; then sanitise. Never throws.
static std::string resolve_and_sanitize_name(feeds_iso_recorder *rec)
{
	std::string name;
	if (rec->name_fn) {
		try {
			name = rec->name_fn(rec->name_ud);
		} catch (...) {
			name.clear();
		}
	}
	if (name.empty()) {
		const char *sn = obs_source_get_name(rec->parent);
		if (sn)
			name = sn;
	}
	if (name.empty())
		name = NAME_FALLBACK;
	return sanitize_filename(name);
}

// Build the full output path in `dir` with extension `ext`. The name hook
// supplies the whole base name (the ISO registry puts the recording stamp and
// offset in it), so nothing is appended except, if that file already exists,
// " (2)", " (3)"... so an ISO never overwrites anything.
static std::string build_filepath(feeds_iso_recorder *rec, const char *dir, const char *ext)
{
	const std::string name = resolve_and_sanitize_name(rec);
	std::string base = dir ? dir : "";
	if (!base.empty() && base.back() != '/' && base.back() != '\\')
		base += '/';
	base += name;

	std::string out = base + "." + ext;
	for (int n = 2; os_file_exists(out.c_str()) && n < 1000; n++)
		out = base + " (" + std::to_string(n) + ")." + ext;
	return out;
}

// ---------------------------------------------------------------------------
// Isolated participant audio (see the file header for the model).
// ---------------------------------------------------------------------------

// Timeline position of an os_gettime_ns() instant. Caller holds audio_m.
static int64_t iso_audio_pos_of(const feeds_iso_recorder *rec, uint64_t ts)
{
	if (ts <= rec->audio_base_ts)
		return 0;
	return (int64_t)util_mul_div64(ts - rec->audio_base_ts, rec->audio_rate, 1000000000ULL);
}

static int64_t iso_audio_frames_of(const feeds_iso_recorder *rec, uint64_t ns)
{
	return (int64_t)util_mul_div64(ns, rec->audio_rate, 1000000000ULL);
}

// audio_output input callback (libobs audio thread). Emits the next block of
// the timeline, ISO_AUDIO_LATENCY_NS behind real time, then zeroes it so the
// buffer slot reads as silence when it comes round again. The audio thread
// advances start_ts by exactly one block per call, so emit_pos advancing by
// one block keeps the two in lockstep; the true time of the block goes back
// through new_ts.
static bool iso_audio_input(void *param, uint64_t start_ts, uint64_t end_ts, uint64_t *new_ts,
			    uint32_t active_mixers, struct audio_output_data *mixes)
{
	UNUSED_PARAMETER(end_ts);
	auto *rec = static_cast<feeds_iso_recorder *>(param);
	const uint64_t ts = start_ts - ISO_AUDIO_LATENCY_NS;
	*new_ts = ts;

	std::lock_guard<std::mutex> lk(rec->audio_m);
	if (rec->audio_ring.empty())
		return true;  // output is silence (mix buffers arrive zeroed)
	if (rec->audio_emit_pos < 0)
		rec->audio_emit_pos = iso_audio_pos_of(rec, ts);

	const size_t mask = rec->audio_ring.size() - 1;
	float *dst = (active_mixers & 1) ? mixes[0].data[0] : nullptr;
	for (size_t i = 0; i < (size_t)AUDIO_OUTPUT_FRAMES; i++) {
		float &s = rec->audio_ring[(size_t)(rec->audio_emit_pos + (int64_t)i) & mask];
		if (dst)
			dst[i] = s;
		s = 0.0f;
	}
	rec->audio_emit_pos += AUDIO_OUTPUT_FRAMES;
	return true;
}

// Write resampled samples at timeline position pos, clipped to the part of
// the buffer that is still ahead of the callback. Caller holds audio_m.
static void iso_audio_write_locked(feeds_iso_recorder *rec, const float *src, uint32_t n, int64_t pos)
{
	const int64_t cap = (int64_t)rec->audio_ring.size();
	const int64_t floor_pos = rec->audio_emit_pos >= 0
					  ? rec->audio_emit_pos
					  : iso_audio_pos_of(rec, os_gettime_ns() - ISO_AUDIO_LATENCY_NS);

	if (pos < floor_pos) {  // already emitted (or about to be): too late
		const int64_t skip = floor_pos - pos;
		if (skip >= (int64_t)n) {
			rec->audio_late_frames += n;
			return;
		}
		rec->audio_late_frames += (uint64_t)skip;
		src += skip;
		n -= (uint32_t)skip;
		pos = floor_pos;
	}
	if (pos + (int64_t)n > floor_pos + cap) {  // beyond the buffer: can't hold it
		const int64_t room = floor_pos + cap - pos;
		const uint32_t keep = room > 0 ? (uint32_t)room : 0;
		rec->audio_ahead_frames += n - keep;
		n = keep;
	}

	const size_t mask = rec->audio_ring.size() - 1;
	for (uint32_t i = 0; i < n; i++)
		rec->audio_ring[(size_t)(pos + (int64_t)i) & mask] = src[i];
}

// Place one engine chunk on the timeline. Caller holds audio_m and has
// checked rec->audio.
static void iso_audio_place_locked(feeds_iso_recorder *rec, const int16_t *pcm, uint32_t frames,
				   uint32_t sample_rate, uint32_t channels, uint64_t timestamp_ns)
{
	if (!rec->audio_logged_first) {
		rec->audio_logged_first = true;
		blog(LOG_INFO, "[feeds-iso] isolated audio arriving for '%s' (%u Hz, %u ch)",
		     rec->audio_name.c_str(), sample_rate, channels);
	}
	rec->audio_received_ns += util_mul_div64(frames, 1000000000ULL, sample_rate);

	// Continue the previous chunk, start a new anchor after a gap, or drop a
	// chunk that is behind where we already are (never rewind).
	const int64_t target = iso_audio_pos_of(rec, timestamp_ns);
	const int64_t smooth = iso_audio_frames_of(rec, ISO_AUDIO_SMOOTH_NS);
	bool contiguous = false;
	if (rec->audio_next_pos >= 0) {
		const int64_t diff = target - rec->audio_next_pos;
		if (diff < -smooth) {
			rec->audio_ahead_frames += (uint64_t)iso_audio_frames_of(
				rec, util_mul_div64(frames, 1000000000ULL, sample_rate));
			return;
		}
		contiguous = diff <= smooth;
	}

	// Format conversion only: s16 at the SDK's rate/channels -> float mono at
	// the output rate. A fresh anchor gets a fresh resampler so no tail of the
	// previous talk spurt is carried across the silence.
	if (!contiguous || !rec->resampler || rec->rs_rate != sample_rate || rec->rs_channels != channels) {
		audio_resampler_destroy(rec->resampler);
		struct resample_info src = {};
		src.samples_per_sec = sample_rate;
		src.format = AUDIO_FORMAT_16BIT;
		src.speakers = channels == 2 ? SPEAKERS_STEREO : SPEAKERS_MONO;
		struct resample_info dst = {};
		dst.samples_per_sec = rec->audio_rate;
		dst.format = AUDIO_FORMAT_FLOAT_PLANAR;
		dst.speakers = SPEAKERS_MONO;
		rec->resampler = audio_resampler_create(&dst, &src);
		rec->rs_rate = sample_rate;
		rec->rs_channels = channels;
		if (!rec->resampler) {
			blog(LOG_WARNING, "[feeds-iso] could not create audio resampler (%u Hz, %u ch)",
			     sample_rate, channels);
			return;
		}
	}

	uint8_t *out[MAX_AV_PLANES] = {};
	uint32_t out_frames = 0;
	uint64_t ts_offset = 0;
	const uint8_t *in[MAX_AV_PLANES] = {reinterpret_cast<const uint8_t *>(pcm)};
	if (!audio_resampler_resample(rec->resampler, out, &out_frames, &ts_offset, in, frames) || !out_frames)
		return;

	// The resampler delays its output by ts_offset; account for it on a fresh
	// anchor, as libobs does for source audio.
	const int64_t pos = contiguous ? rec->audio_next_pos
				       : iso_audio_pos_of(rec, timestamp_ns > ts_offset ? timestamp_ns - ts_offset : 0);
	iso_audio_write_locked(rec, reinterpret_cast<const float *>(out[0]), out_frames, pos);
	rec->audio_next_pos = pos + out_frames;
}

// Open this recording's private audio_output. Called from start_output
// (graphics thread, rec->lock held). Returns the audio handle, or null.
static audio_t *iso_audio_open(feeds_iso_recorder *rec)
{
	struct obs_audio_info oai = {};
	if (!obs_get_audio_info(&oai) || !oai.samples_per_sec)
		return nullptr;

	// Buffer: the smallest power of two covering ISO_AUDIO_BUFFER_NS.
	size_t ring = 1;
	const uint64_t need = util_mul_div64(ISO_AUDIO_BUFFER_NS, oai.samples_per_sec, 1000000000ULL);
	while (ring < need)
		ring <<= 1;

	{
		std::lock_guard<std::mutex> lk(rec->audio_m);
		rec->audio_rate = oai.samples_per_sec;
		// Origin a second in the past so no plausible stamp maps below zero.
		rec->audio_base_ts = os_gettime_ns() - 1000000000ULL;
		rec->audio_ring.assign(ring, 0.0f);
		rec->audio_next_pos = -1;
		rec->audio_emit_pos = -1;
		rec->audio_logged_first = false;
		rec->audio_received_ns = 0;
		rec->audio_late_frames = 0;
		rec->audio_ahead_frames = 0;
	}

	struct audio_output_info info = {};
	info.name = rec->audio_name.c_str();
	info.samples_per_sec = oai.samples_per_sec;
	info.format = AUDIO_FORMAT_FLOAT_PLANAR;
	info.speakers = SPEAKERS_MONO;
	info.input_callback = iso_audio_input;
	info.input_param = rec;

	audio_t *audio = nullptr;
	if (audio_output_open(&audio, &info) != AUDIO_OUTPUT_SUCCESS || !audio) {
		std::lock_guard<std::mutex> lk(rec->audio_m);
		rec->audio_ring.clear();
		return nullptr;
	}

	std::lock_guard<std::mutex> lk(rec->audio_m);
	rec->audio = audio;  // push_audio starts accepting chunks
	return audio;
}

// Close the private audio_output, if open. Must run after the encoder bound to
// it has been released. audio_output_close joins the audio thread, so once it
// returns no callback can touch the buffer being freed below.
static void iso_audio_close(feeds_iso_recorder *rec)
{
	audio_t *audio = nullptr;
	{
		std::lock_guard<std::mutex> lk(rec->audio_m);
		audio = rec->audio;
		rec->audio = nullptr;  // push_audio stops accepting chunks
	}
	if (!audio)
		return;
	audio_output_close(audio);

	std::lock_guard<std::mutex> lk(rec->audio_m);
	blog(LOG_INFO,
	     "[feeds-iso] isolated audio closed for '%s': %llu ms received, %llu ms dropped late, "
	     "%llu ms dropped ahead",
	     rec->audio_name.c_str(), (unsigned long long)(rec->audio_received_ns / 1000000ULL),
	     (unsigned long long)util_mul_div64(rec->audio_late_frames, 1000ULL, rec->audio_rate),
	     (unsigned long long)util_mul_div64(rec->audio_ahead_frames, 1000ULL, rec->audio_rate));
	audio_resampler_destroy(rec->resampler);
	rec->resampler = nullptr;
	rec->rs_rate = 0;
	rec->rs_channels = 0;
	rec->audio_ring.clear();
	rec->audio_ring.shrink_to_fit();
	rec->audio_next_pos = -1;
	rec->audio_emit_pos = -1;
}

void feeds_iso_recorder_push_audio(feeds_iso_recorder *rec, const int16_t *pcm, uint32_t frames,
				   uint32_t sample_rate, uint32_t channels, uint64_t timestamp_ns)
{
#if FEEDS_ISO_ISOLATED_AUDIO
	if (!rec || !pcm || !frames || !sample_rate || (channels != 1 && channels != 2))
		return;
	std::lock_guard<std::mutex> lk(rec->audio_m);
	if (!rec->audio)
		return;  // not recording
	iso_audio_place_locked(rec, pcm, frames, sample_rate, channels, timestamp_ns);
#else
	UNUSED_PARAMETER(rec);
	UNUSED_PARAMETER(pcm);
	UNUSED_PARAMETER(frames);
	UNUSED_PARAMETER(sample_rate);
	UNUSED_PARAMETER(channels);
	UNUSED_PARAMETER(timestamp_ns);
#endif
}

// Defined in the teardown section below; used by start_output's failure path.
static void detach_view_source(feeds_iso_recorder *rec);

// ---------------------------------------------------------------------------
// Start: build the output + encoders from OBS's main-recording config and
// start recording. Called from tick (graphics thread) with rec->lock held,
// once video_output is ready. Mirrors Source Record's start_file_output +
// update_encoder, trimmed to a single muxer output and one audio track.
// ---------------------------------------------------------------------------
static void start_output(feeds_iso_recorder *rec)
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return;

	// Base name for the encoder/output labels (libobs doesn't require these
	// to be unique, but distinct names keep multi-ISO logs readable).
	const char *sname = obs_source_get_name(rec->parent);
	std::string base = (sname && *sname) ? sname : "feeds_iso";
	std::string venc_name = base + " (ISO video)";
	std::string aenc_name = base + " (ISO audio)";
	std::string out_name = base + " (ISO)";

	const char *mode = config_get_string(cfg, "Output", "Mode");
	const bool advanced = mode && (strcmp(mode, "Advanced") == 0 || strcmp(mode, "advanced") == 0);
	const char *section = advanced ? "AdvOut" : "SimpleOutput";

	// Format -> extension + muxer. Prefer RecFormat2, fall back to RecFormat.
	const char *format = config_get_string(cfg, section, "RecFormat2");
	if (!format || !*format)
		format = config_get_string(cfg, section, "RecFormat");
	const char *ext = format_extension(format);
	const char *output_id = format_output_id(format);

	// Recording folder. OBS populates this default at profile creation, so
	// it's effectively always set; the fallback to the OBS config dir (an
	// always-writable location) only guards the pathological empty case.
	const char *cfg_dir = advanced ? config_get_string(cfg, "AdvOut", "RecFilePath")
				       : config_get_string(cfg, "SimpleOutput", "FilePath");
	std::string dir;
	if (cfg_dir && *cfg_dir) {
		dir = cfg_dir;
	} else {
		char *fallback = os_get_config_path_ptr(nullptr);
		if (fallback) {
			dir = fallback;
			bfree(fallback);
		}
		blog(LOG_WARNING, "[feeds-iso] no recording path configured; using '%s'", dir.c_str());
	}

	// Video encoder ID.
	std::string venc_id;
	if (advanced) {
		const char *e = config_get_string(cfg, "AdvOut", "RecEncoder");
		if (!e || !*e || strcmp(e, "none") == 0 || strcmp(e, "None") == 0)
			e = config_get_string(cfg, "AdvOut", "Encoder");  // fall to stream encoder
		venc_id = (e && *e) ? e : "obs_x264";
	} else {
		venc_id = simple_to_canonical_encoder(config_get_string(cfg, "SimpleOutput", "RecEncoder"));
	}

	// Encoder settings. Advanced mode is exact (recordEncoder.json); Simple
	// mode uses the encoder's built-in OBS defaults (per the locked design
	// decision — no RecQuality/VBitrate replication).
	obs_data_t *venc_settings = nullptr;
	if (advanced) {
		char *profile_path = obs_frontend_get_current_profile_path();
		if (profile_path) {
			std::string json = std::string(profile_path) + "/recordEncoder.json";
			venc_settings = obs_data_create_from_json_file(json.c_str());
			bfree(profile_path);
		}
	}

	// Create the video encoder; on failure (e.g. an unavailable HW encoder)
	// fall back to x264 so a recording still happens.
	rec->venc = obs_video_encoder_create(venc_id.c_str(), venc_name.c_str(), venc_settings, nullptr);
	if (!rec->venc)
		rec->venc = obs_video_encoder_create("obs_x264", venc_name.c_str(), venc_settings, nullptr);
	obs_data_release(venc_settings);
	if (!rec->venc) {
		blog(LOG_ERROR, "[feeds-iso] failed to create video encoder");
		return;
	}
	obs_encoder_set_video(rec->venc, rec->video_output);

	// Audio encoder — AAC. On Windows it is bound to this recording's PRIVATE
	// audio_output, which carries only this source's participant (see the
	// file header); it is ours, and is closed after the encoder is released
	// (finish_drain). If that pipeline can't open we record video only rather
	// than fall back to the program mix, which would put every voice in every
	// ISO file. On macOS (no isolated audio yet) the encoder binds to
	// obs_get_audio(), libobs's program mix (mix 0 = OBS recording track 1),
	// which libobs owns and we never close.
	audio_t *audio = nullptr;
#if FEEDS_ISO_ISOLATED_AUDIO
	rec->audio_name = base;
	audio = iso_audio_open(rec);
	if (!audio)
		blog(LOG_ERROR, "[feeds-iso] could not open the isolated audio pipeline for '%s'; "
				"recording video only", base.c_str());
#else
	audio = obs_get_audio();
#endif
	if (audio) {
		obs_data_t *aenc_settings = obs_data_create();
		obs_data_set_int(aenc_settings, "bitrate", AUDIO_BITRATE_KBPS);
		rec->aenc = obs_audio_encoder_create("ffmpeg_aac", aenc_name.c_str(), aenc_settings, 0, nullptr);
		obs_data_release(aenc_settings);
		if (rec->aenc)
			obs_encoder_set_audio(rec->aenc, audio);
		else
			iso_audio_close(rec);  // nothing to feed; no-op on macOS
	}

	// Build the output and its settings (path).
	std::string path = build_filepath(rec, dir.c_str(), ext);
	obs_data_t *out_settings = obs_data_create();
	obs_data_set_string(out_settings, "path", path.c_str());
	rec->output = obs_output_create(output_id, out_name.c_str(), out_settings, nullptr);
	obs_data_release(out_settings);
	if (!rec->output) {
		blog(LOG_ERROR, "[feeds-iso] failed to create output (%s)", output_id);
		obs_encoder_release(rec->venc);
		rec->venc = nullptr;
		obs_encoder_release(rec->aenc);
		rec->aenc = nullptr;
		iso_audio_close(rec);  // after its encoder
		return;
	}

	obs_output_set_video_encoder(rec->output, rec->venc);
	if (rec->aenc)
		obs_output_set_audio_encoder(rec->output, rec->aenc, 0);

	// Point the view's source channel at the parent so the view renders it.
	// obs_view_set_source activates the source for the view, which makes an
	// otherwise-invisible (but subscribed) async source tick/render into our
	// private view. Balanced by detach_view_source on stop.
	obs_source_t *cur = obs_view_get_source(rec->view, 0);
	if (cur != rec->parent)
		obs_view_set_source(rec->view, 0, rec->parent);
	obs_source_release(cur);

	if (obs_output_start(rec->output)) {
		rec->output_active = true;
		rec->paused = false;
		blog(LOG_INFO, "[feeds-iso] recording started: %s", path.c_str());
		if (rec->started_fn) {
			try {
				rec->started_fn(rec->name_ud, path);
			} catch (...) {
			}
		}
	} else {
		blog(LOG_ERROR, "[feeds-iso] obs_output_start failed: %s", obs_output_get_last_error(rec->output));
		detach_view_source(rec);
		obs_output_release(rec->output);
		rec->output = nullptr;
		obs_encoder_release(rec->venc);
		rec->venc = nullptr;
		obs_encoder_release(rec->aenc);
		rec->aenc = nullptr;
		iso_audio_close(rec);  // after its encoder
	}
}

// ---------------------------------------------------------------------------
// Detach the parent from the render view (which deactivates it). Caller holds
// rec->lock. Used by both the async per-recording stop and the synchronous
// destroy. Idempotent — obs_view's render loop may already have dropped a
// removed parent, in which case the channel is already NULL and this no-ops.
// ---------------------------------------------------------------------------
static void detach_view_source(feeds_iso_recorder *rec)
{
	if (!rec->view)
		return;
	obs_source_t *cur = obs_view_get_source(rec->view, 0);
	if (cur) {
		obs_view_set_source(rec->view, 0, nullptr);
		obs_source_release(cur);
	}
}

// ---------------------------------------------------------------------------
// Teardown (Bugs 3, 4, 5).
//
// Stopping is split into two phases so the drain is graceful (Bug 4) without
// blocking the caller and without the busy-poll that hangs OBS (Bug 5):
//
//   request_stop  — tell the output to drain gracefully (obs_output_stop) and
//                   mark the recorder "draining". Returns immediately. The
//                   output's "stop" signal (fired on an internal OBS thread
//                   once the muxer has flushed) sets drain_done.
//   finish_drain  — release the drained output + encoders in strict order
//                   (Bug 3: output before encoders). Runs on a core thread:
//                   from tick once drain_done is set / the deadline lapses
//                   (normal stops), or inline from destroy after a bounded
//                   wait. Never a busy-loop.
//
// Only one output drains at a time; tick blocks new starts and view
// recreation while draining, so a draining output never has its mix freed
// underneath it. The persistent view is torn down only at destroy, after the
// final drain has been released. The private isolated-audio audio_output is
// per recording: finish_drain closes it after the audio encoder is released.
// ---------------------------------------------------------------------------

// "stop" signal handler: marks the drain complete and wakes destroy's waiter.
// Connected with data = rec; finish_drain disconnects it, and the recorder
// outlives every firing (normal stop: rec alive; destroy: waits before free).
static void on_drain_done(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	auto *rec = static_cast<feeds_iso_recorder *>(data);
	rec->drain_done.store(true);
	{
		std::lock_guard<std::mutex> lk(rec->drain_m);
	}
	rec->drain_cv.notify_all();
}

// Begin a graceful stop. Caller holds rec->lock. No-op if not recording or
// already draining (only one drain at a time).
static void request_stop(feeds_iso_recorder *rec)
{
	if (!rec->output || rec->draining)
		return;

	detach_view_source(rec);  // stop feeding new frames; deactivates the parent
	rec->output_active = false;
	rec->paused = false;
	rec->draining = true;
	rec->drain_forced = false;
	rec->drain_done.store(false);
	rec->drain_deadline_ns = os_gettime_ns() + (uint64_t)TEARDOWN_TIMEOUT_MS * 1000000ULL;

	signal_handler_t *sh = obs_output_get_signal_handler(rec->output);
	if (sh)
		signal_handler_connect(sh, "stop", on_drain_done, rec);
	else
		rec->drain_done.store(true);  // can't await; finish on next tick

	obs_output_stop(rec->output);  // graceful drain (Bug 4)
}

// Release the drained output + encoders in strict order (Bug 3). Caller holds
// rec->lock and must have confirmed the drain is complete (or force-stopped).
static void finish_drain(feeds_iso_recorder *rec)
{
	if (rec->output) {
		signal_handler_t *sh = obs_output_get_signal_handler(rec->output);
		if (sh)
			signal_handler_disconnect(sh, "stop", on_drain_done, rec);
		obs_output_release(rec->output);
		rec->output = nullptr;
	}
	if (rec->venc) {
		obs_encoder_release(rec->venc);
		rec->venc = nullptr;
	}
	if (rec->aenc) {
		obs_encoder_release(rec->aenc);
		rec->aenc = nullptr;
	}
	iso_audio_close(rec);  // the private audio_output, after its encoder
	rec->draining = false;
	rec->drain_forced = false;
	rec->drain_done.store(false);
}

// Advance a drain in progress (called from tick, rec->lock held). Releases the
// output once "stop" has fired, or force-stops once past the deadline (Bug 5),
// then releases on the following tick. Returns true while still draining.
static bool service_drain(feeds_iso_recorder *rec)
{
	if (!rec->draining)
		return false;
	if (rec->drain_done.load()) {
		finish_drain(rec);
		return false;
	}
	if (os_gettime_ns() > rec->drain_deadline_ns) {
		if (!rec->drain_forced && rec->output) {
			blog(LOG_WARNING, "[feeds-iso] output did not drain within %d ms; forcing stop",
			     TEARDOWN_TIMEOUT_MS);
			obs_output_force_stop(rec->output);  // emits "stop" shortly -> drain_done
			rec->drain_forced = true;
			rec->drain_deadline_ns = os_gettime_ns() + 2000ULL * 1000000ULL;
		} else if (rec->drain_forced) {
			// Forced stop didn't signal in time; release anyway (best
			// effort) rather than leak the output indefinitely.
			finish_drain(rec);
			return false;
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Tick — runs on the graphics thread. Maintains the render view and drives
// start/stop based on want_record/enabled.
// ---------------------------------------------------------------------------
void feeds_iso_recorder_tick(feeds_iso_recorder *rec, float seconds)
{
	UNUSED_PARAMETER(seconds);
	if (!rec || rec->closing.load())
		return;

	std::lock_guard<std::mutex> lk(rec->lock);
	if (!rec->parent || obs_source_removed(rec->parent))
		return;

	// Service a drain in progress first. While draining we don't start a new
	// recording or recreate the view, so the draining output keeps its mix.
	if (service_drain(rec))
		return;

	const bool should_record = rec->want_record && rec->enabled && rec->tier >= 1;

	// Recording dimensions: the fixed size if one is set (the file never
	// restarts on a parent size change, and records black until the parent has
	// a picture), else the parent's current size. Rounded up to even (H.264
	// requires even dimensions — Source Record:1339-1342).
	uint32_t width = rec->fixed_width ? rec->fixed_width : obs_source_get_width(rec->parent);
	width += (width & 1);
	uint32_t height = rec->fixed_height ? rec->fixed_height : obs_source_get_height(rec->parent);
	height += (height & 1);

	// Dimension change while recording: stop the current output. The next
	// tick (after the drain) recreates the view at the new size and restarts.
	if (rec->output_active && width && height && (rec->width != width || rec->height != height)) {
		request_stop(rec);
		return;  // want_record stays set, so it restarts after the drain
	}

	// (Re)create the render view when dimensions become valid or change. Safe
	// here because we're not draining and (if recording) dimensions match.
	if (width && height && (!rec->video_output || rec->width != width || rec->height != height)) {
		struct obs_video_info ovi = {};
		obs_get_video_info(&ovi);
		ovi.base_width = width;
		ovi.base_height = height;
		ovi.output_width = width;
		ovi.output_height = height;

		if (!rec->view)
			rec->view = obs_view_create();
		if (rec->video_output)
			obs_view_remove(rec->view);
		rec->video_output = obs_view_add2(rec->view, &ovi);
		if (rec->video_output) {
			rec->width = width;
			rec->height = height;
		}
	}

	if (should_record && !rec->output_active && rec->video_output && width && height) {
		start_output(rec);
		// If start failed (bad config / unavailable encoder), don't retry
		// every frame — wait for the next recording-start event or a
		// re-enable. start_output already logged the cause.
		if (!rec->output_active)
			rec->want_record = false;
	} else if (!should_record && rec->output_active) {
		request_stop(rec);
	}

	// Re-assert pause state to mirror the main recording (covers an output
	// that started after a pause event arrived). Source Record:885-892.
	if (rec->output_active && rec->output) {
		if (rec->paused && !obs_output_paused(rec->output))
			obs_output_pause(rec->output, true);
		else if (!rec->paused && obs_output_paused(rec->output))
			obs_output_pause(rec->output, false);
	}
}

// ---------------------------------------------------------------------------
// Enable / tier
// ---------------------------------------------------------------------------
void feeds_iso_recorder_set_enabled(feeds_iso_recorder *rec, bool enabled)
{
	if (!rec)
		return;
	std::lock_guard<std::mutex> lk(rec->lock);
	if (enabled == rec->enabled)
		return;
	rec->enabled = enabled;

	if (enabled) {
		// Bug 6: clear the stuck-state sentinel so the next frontend
		// event is honoured, and start immediately if OBS is already
		// recording (tick performs the actual start once the view is
		// ready).
		rec->last_frontend_event = -1;
		if (rec->tier >= 1 && obs_frontend_recording_active())
			rec->want_record = true;
	} else {
		rec->want_record = false;
		if (rec->output_active)
			request_stop(rec);
	}
}

bool feeds_iso_recorder_is_enabled(const feeds_iso_recorder *rec)
{
	return rec && rec->enabled;
}

void feeds_iso_recorder_set_tier(feeds_iso_recorder *rec, int tier)
{
	if (!rec)
		return;
	std::lock_guard<std::mutex> lk(rec->lock);
	rec->tier = tier;
	if (tier < 1) {
		rec->want_record = false;
		if (rec->output_active)
			request_stop(rec);
	}
}

// ---------------------------------------------------------------------------
// Frontend recording event hooks (invoked by the module dispatcher below).
// ---------------------------------------------------------------------------
void feeds_iso_recorder_on_obs_recording_started(feeds_iso_recorder *rec)
{
	if (!rec)
		return;
	std::lock_guard<std::mutex> lk(rec->lock);
	rec->last_frontend_event = OBS_FRONTEND_EVENT_RECORDING_STARTED;
	if (rec->enabled && rec->tier >= 1)
		rec->want_record = true;  // tick starts once the view is ready
}

void feeds_iso_recorder_on_obs_recording_stopping(feeds_iso_recorder *rec)
{
	if (!rec)
		return;
	std::lock_guard<std::mutex> lk(rec->lock);
	rec->last_frontend_event = OBS_FRONTEND_EVENT_RECORDING_STOPPING;
	rec->want_record = false;
	if (rec->output_active)
		request_stop(rec);  // graceful drain (Bug 4)
}

void feeds_iso_recorder_on_obs_recording_paused(feeds_iso_recorder *rec)
{
	if (!rec)
		return;
	std::lock_guard<std::mutex> lk(rec->lock);
	rec->last_frontend_event = OBS_FRONTEND_EVENT_RECORDING_PAUSED;
	rec->paused = true;
	if (rec->output_active && rec->output && !obs_output_paused(rec->output))
		obs_output_pause(rec->output, true);
}

void feeds_iso_recorder_on_obs_recording_unpaused(feeds_iso_recorder *rec)
{
	if (!rec)
		return;
	std::lock_guard<std::mutex> lk(rec->lock);
	rec->last_frontend_event = OBS_FRONTEND_EVENT_RECORDING_UNPAUSED;
	rec->paused = false;
	if (rec->output_active && rec->output && obs_output_paused(rec->output))
		obs_output_pause(rec->output, false);
}

// ---------------------------------------------------------------------------
// Single frontend-event callback — fans out to every registered recorder.
// ---------------------------------------------------------------------------
static void frontend_event(enum obs_frontend_event event, void *)
{
	std::lock_guard<std::mutex> lk(g_registry_mutex);
	switch (event) {
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		for (auto *rec : g_registry)
			feeds_iso_recorder_on_obs_recording_started(rec);
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPING:
		for (auto *rec : g_registry)
			feeds_iso_recorder_on_obs_recording_stopping(rec);
		break;
	case OBS_FRONTEND_EVENT_RECORDING_PAUSED:
		for (auto *rec : g_registry)
			feeds_iso_recorder_on_obs_recording_paused(rec);
		break;
	case OBS_FRONTEND_EVENT_RECORDING_UNPAUSED:
		for (auto *rec : g_registry)
			feeds_iso_recorder_on_obs_recording_unpaused(rec);
		break;
	default:
		break;
	}
}

// ---------------------------------------------------------------------------
// Create / destroy
// ---------------------------------------------------------------------------
feeds_iso_recorder *feeds_iso_recorder_create(obs_source_t *parent_source, feeds_iso_name_fn name_fn,
					      feeds_iso_started_fn started_fn, void *userdata)
{
	auto *rec = new feeds_iso_recorder();
	rec->parent = parent_source;
	rec->name_fn = name_fn;
	rec->started_fn = started_fn;
	rec->name_ud = userdata;

	std::lock_guard<std::mutex> lk(g_registry_mutex);
	g_registry.push_back(rec);
	return rec;
}

void feeds_iso_recorder_set_fixed_size(feeds_iso_recorder *rec, uint32_t width, uint32_t height)
{
	if (!rec)
		return;
	std::lock_guard<std::mutex> lk(rec->lock);
	rec->fixed_width = width;
	rec->fixed_height = height;
}

void feeds_iso_recorder_destroy(feeds_iso_recorder *rec)
{
	if (!rec)
		return;

	// Remove from the registry first so the frontend dispatcher can't touch
	// rec once we start tearing it down.
	{
		std::lock_guard<std::mutex> lk(g_registry_mutex);
		for (size_t i = 0; i < g_registry.size(); i++) {
			if (g_registry[i] == rec) {
				g_registry.erase(g_registry.begin() + i);
				break;
			}
		}
	}

	// closing short-circuits new ticks and the audio callback; the lock
	// barrier then waits for any in-flight tick to finish. After this, rec is
	// owned solely by this thread (the frontend dispatcher no longer sees it,
	// and no new tick proceeds), so the steps below run lock-free.
	rec->closing.store(true);
	{
		std::lock_guard<std::mutex> barrier(rec->lock);
	}

	// Destruction is SYNCHRONOUS: the borrowed parent pointer is valid only
	// until this destroy callback returns, and the recording pipeline must be
	// gone before rec is freed (the isolated-audio thread reads rec). So we
	// drain + release everything inline, with a bounded wait, before
	// returning; finish_drain closes the audio_output. The parent stays valid throughout (we're inside
	// the source's destroy callback) and the view holds it until detached.

	// Begin a graceful drain of an active recording (no-op if already
	// draining from a prior stop, or not recording).
	if (rec->output && !rec->draining)
		request_stop(rec);  // graceful drain (Bug 4)

	// Wait (bounded) for the drain, force-stopping once on timeout (Bug 5),
	// then release the output + encoders inline.
	if (rec->draining) {
		std::unique_lock<std::mutex> wl(rec->drain_m);
		if (!rec->drain_cv.wait_for(wl, std::chrono::milliseconds(TEARDOWN_TIMEOUT_MS),
					    [rec] { return rec->drain_done.load(); })) {
			wl.unlock();
			blog(LOG_WARNING, "[feeds-iso] destroy: output did not drain in %d ms; forcing stop",
			     TEARDOWN_TIMEOUT_MS);
			if (rec->output)
				obs_output_force_stop(rec->output);
			wl.lock();
			rec->drain_cv.wait_for(wl, std::chrono::milliseconds(2000), [rec] { return rec->drain_done.load(); });
			wl.unlock();
		} else {
			wl.unlock();
		}
		finish_drain(rec);  // disconnect "stop", release output + encoders (Bug 3)
	}

	// Belt and braces: no recording audio pipeline may outlive rec. (finish_drain
	// has normally closed it already; this is a no-op then.)
	iso_audio_close(rec);

	// Release the persistent render view.
	if (rec->view) {
		obs_view_set_source(rec->view, 0, nullptr);
		obs_view_remove(rec->view);
		obs_view_destroy(rec->view);
		rec->view = nullptr;
		rec->video_output = nullptr;
	}

	delete rec;
}

// ---------------------------------------------------------------------------
// Module wiring
// ---------------------------------------------------------------------------
void feeds_iso_recorder_module_load()
{
	obs_frontend_add_event_callback(frontend_event, nullptr);
}

void feeds_iso_recorder_module_unload()
{
	obs_frontend_remove_event_callback(frontend_event, nullptr);

	// Drain anything still active. In practice the registry is already empty
	// here — OBS destroys sources (and thus their recorders, synchronously)
	// before unloading modules — so this is belt-and-suspenders.
	std::vector<feeds_iso_recorder *> remaining;
	{
		std::lock_guard<std::mutex> lk(g_registry_mutex);
		remaining = g_registry;
	}
	for (auto *rec : remaining)
		feeds_iso_recorder_destroy(rec);
}

}  // namespace feeds
