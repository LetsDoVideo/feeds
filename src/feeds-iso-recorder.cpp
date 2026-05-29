// feeds-iso-recorder.cpp — per-source ISO recording for Feeds participant
// sources (Phase 1). See feeds-iso-recorder.h and
// C:\Dev\iso-recording-investigation.md for the full design rationale.
//
// This adopts Exeldro's Source Record patterns (C:\Dev\obs-source-record\
// source-record.c) — a private obs_view for video and an obs_output muxer,
// with audio taken from OBS's program mix (obs_get_audio(), per Source
// Record's program-audio path) — used DIRECTLY inside the participant source
// rather than as an OBS filter, with the six known Source Record bugs fixed:
//
//   Bug 1: use-after-release in Source Record's audio callback. Not
//          applicable to Feeds: the audio encoder binds directly to OBS's
//          program mix (obs_get_audio()), so there is no per-source audio
//          callback here and the buggy code path doesn't exist.
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
// Threading: source lifecycle + frontend events arrive on the OBS UI thread;
// tick runs on the graphics thread. Both mutate the recorder, so the state
// machine is guarded by feeds_iso_recorder::lock. Audio needs no per-source
// callback and no lock: the audio encoder is bound directly to OBS's program
// mix (obs_get_audio()), so libobs's own audio plumbing routes the samples
// into the recording. The output's "stop" signal fires on an internal OBS
// thread and only sets an atomic flag + notifies a CV; the actual release
// happens on a core thread (tick, or destroy after a bounded wait).

#include "feeds-iso-recorder.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/platform.h>

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

// Audio bitrate (kbps) for the AAC track. The track carries OBS's full program
// mix (Phase 1.1), so this is a real, audible bitrate.
constexpr int AUDIO_BITRATE_KBPS = 160;

}  // namespace

// ---------------------------------------------------------------------------
// Recorder state
// ---------------------------------------------------------------------------
struct feeds_iso_recorder {
	obs_source_t *parent = nullptr;  // borrowed; stable for the recorder's life
	feeds_iso_name_fn name_fn = nullptr;
	void *name_ud = nullptr;

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

	bool enabled = false;       // checkbox state
	bool want_record = false;   // OBS main recording active and we should record
	bool paused = false;        // desired pause state, mirrors main recording
	std::atomic<bool> closing{false};  // teardown in progress (read by audio thread)
	int tier = 0;
	int last_frontend_event = -1;  // -1 == "none" sentinel (Bug 6)
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

// Build the full output path in `dir` with extension `ext`.
static std::string build_filepath(feeds_iso_recorder *rec, const char *dir, const char *ext)
{
	std::string name = resolve_and_sanitize_name(rec);
	// os_generate_formatted_filename already appends ".<ext>", so the result is
	// "<name> 2026-05-27 14-03-09.<ext>" once we prepend the sanitized name.
	char *ts = os_generate_formatted_filename(ext, true, "%CCYY-%MM-%DD %hh-%mm-%ss");
	std::string filename;
	if (ts) {
		filename = name + " " + ts;
		bfree(ts);
	} else {
		filename = name + " recording." + ext;
	}

	std::string out = dir ? dir : "";
	if (!out.empty() && out.back() != '/' && out.back() != '\\')
		out += '/';
	out += filename;
	return out;
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

	// Audio encoder — AAC bound to OBS's program audio mix. obs_get_audio()
	// is libobs's global program audio handle (the same one feeding the main
	// recording); mix index 0 = OBS recording track 1. Binding the encoder to
	// it lets libobs's own audio plumbing route the full program mix into the
	// ISO file, so it carries the same audio as the main recording. This
	// handle is owned by libobs core and must NOT be closed by us (see
	// destroy) — we no longer open a private per-source audio_output.
	obs_data_t *aenc_settings = obs_data_create();
	obs_data_set_int(aenc_settings, "bitrate", AUDIO_BITRATE_KBPS);
	rec->aenc = obs_audio_encoder_create("ffmpeg_aac", aenc_name.c_str(), aenc_settings, 0, nullptr);
	obs_data_release(aenc_settings);
	if (rec->aenc)
		obs_encoder_set_audio(rec->aenc, obs_get_audio());

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
	} else {
		blog(LOG_ERROR, "[feeds-iso] obs_output_start failed: %s", obs_output_get_last_error(rec->output));
		detach_view_source(rec);
		obs_output_release(rec->output);
		rec->output = nullptr;
		obs_encoder_release(rec->venc);
		rec->venc = nullptr;
		obs_encoder_release(rec->aenc);
		rec->aenc = nullptr;
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
// final drain has been released. (Audio comes from OBS's shared program mix,
// obs_get_audio(), which we never own and never free.)
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

	// Current parent dimensions, rounded up to even (H.264 requires even
	// dimensions — Source Record:1339-1342).
	uint32_t width = obs_source_get_width(rec->parent);
	width += (width & 1);
	uint32_t height = obs_source_get_height(rec->parent);
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
feeds_iso_recorder *feeds_iso_recorder_create(obs_source_t *parent_source, feeds_iso_name_fn name_fn, void *userdata)
{
	auto *rec = new feeds_iso_recorder();
	rec->parent = parent_source;
	rec->name_fn = name_fn;
	rec->name_ud = userdata;

	std::lock_guard<std::mutex> lk(g_registry_mutex);
	g_registry.push_back(rec);
	return rec;
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
	// until this destroy callback returns, and the audio thread (closed below)
	// references it. So we drain + release everything inline, with a bounded
	// wait, before returning. The parent stays valid throughout (we're inside
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

	// Release the persistent render view. The recording's audio came from
	// OBS's shared program mix (obs_get_audio()), which libobs core owns — we
	// never opened a private audio_output, so there is nothing of ours to
	// close here.
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
