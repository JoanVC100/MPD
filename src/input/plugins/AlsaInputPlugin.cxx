// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

/*
 * ALSA code based on an example by Paul Davis released under GPL here:
 * http://equalarea.com/paul/alsa-audio.html
 * and one by Matthias Nagorni, also GPL, here:
 * http://alsamodular.sourceforge.net/alsa_programming_howto.html
 */

#include "AlsaInputPlugin.hxx"
#include "lib/alsa/NonBlock.hxx"
#include "lib/alsa/Error.hxx"
#include "lib/alsa/Format.hxx"
#include "lib/fmt/ToBuffer.hxx"
#include "../AsyncInputStream.hxx"
#include "event/Call.hxx"
#include "config/Block.hxx"
#include "util/Domain.hxx"
#include "util/StringCompare.hxx"
#include "util/StringSplit.hxx"
#include "pcm/AudioParser.hxx"
#include "pcm/AudioFormat.hxx"
#include "Log.hxx"
#include "event/MultiSocketMonitor.hxx"
#include "event/InjectEvent.hxx"

#include <alsa/asoundlib.h>

#include <fmt/core.h>

#include <cassert>

#include <string.h>

using std::string_view_literals::operator""sv;

static constexpr Domain alsa_input_domain("alsa");

static constexpr auto ALSA_URI_PREFIX = "alsa://";

static constexpr auto BUILTIN_DEFAULT_DEVICE = "default";
static constexpr auto BUILTIN_DEFAULT_FORMAT = "48000:16:2";

static constexpr auto DEFAULT_BUFFER_TIME = std::chrono::milliseconds(1000);
static constexpr auto DEFAULT_RESUME_TIME = DEFAULT_BUFFER_TIME / 2;


static struct {
	EventLoop *event_loop;
	const char *default_device;
	const char *default_format;
	int mode;
} global_config;


class AlsaInputStream final
	: public AsyncInputStream,
	  MultiSocketMonitor {

	/**
	 * The configured name of the ALSA device.
	 */
	const std::string device;

	snd_pcm_t *capture_handle;
	const size_t frame_size;

	Alsa::NonBlockPcm non_block;

	InjectEvent defer_invalidate_sockets;

public:

	class SourceSpec;

	AlsaInputStream(EventLoop &_loop,
			Mutex &_mutex,
			const SourceSpec &spec);

	~AlsaInputStream() override {
		BlockingCall(MultiSocketMonitor::GetEventLoop(), [this](){
				MultiSocketMonitor::Reset();
				defer_invalidate_sockets.Cancel();
			});

		snd_pcm_close(capture_handle);
	}

	AlsaInputStream(const AlsaInputStream &) = delete;
	AlsaInputStream &operator=(const AlsaInputStream &) = delete;

	static InputStreamPtr Create(EventLoop &event_loop, std::string_view uri,
				     Mutex &mutex);

protected:
	/* virtual methods from AsyncInputStream */
	void DoResume() override {
		snd_pcm_resume(capture_handle);

		InvalidateSockets();
	}

	void DoSeek([[maybe_unused]] offset_type new_offset) override {
		/* unreachable because seekable==false */
		SeekDone();
	}

private:
	void OpenDevice(const AudioFormat &audio_format);
	void ConfigureCapture(AudioFormat audio_format);

	void Pause() {
		AsyncInputStream::Pause();
		InvalidateSockets();
	}

	int Recover(int err);

	/* virtual methods from class MultiSocketMonitor */
	Event::Duration PrepareSockets() noexcept override;
	void DispatchSockets() noexcept override;
};


class AlsaInputStream::SourceSpec {
	std::string_view uri;
	std::string_view device_name;
	std::string_view format_string;
	AudioFormat audio_format;

public:
	[[nodiscard]]
	explicit SourceSpec(std::string_view _uri)
		: uri(_uri)
	{
		const auto [a, b] = Split(uri, '?');
		device_name = StringAfterPrefixIgnoreCase(a, ALSA_URI_PREFIX);

		if (b.data() != nullptr)
			format_string = StringAfterPrefixIgnoreCase(b, "format="sv);
		else if (global_config.default_format != nullptr)
			format_string = global_config.default_format;

		if (IsValidScheme()) {
			if (device_name.empty())
				device_name = global_config.default_device;
			if (format_string.data() != nullptr)
				audio_format = ParseAudioFormat(format_string, false);
		}
	}
	[[nodiscard]] bool IsValidScheme() const noexcept {
		return device_name.data() != nullptr;
	}
	[[nodiscard]] bool IsValid() const noexcept {
		return device_name.data() != nullptr && format_string.data() != nullptr;
	}
	[[nodiscard]] std::string_view GetURI() const noexcept {
		return uri;
	}
	[[nodiscard]] std::string_view GetDeviceName() const noexcept {
		return device_name;
	}
	[[nodiscard]] std::string_view GetFormatString() const noexcept {
		return format_string;
	}
	[[nodiscard]] AudioFormat GetAudioFormat() const noexcept {
		return audio_format;
	}
};

AlsaInputStream::AlsaInputStream(EventLoop &_loop,
		Mutex &_mutex,
		const SourceSpec &spec)
	:AsyncInputStream(_loop, spec.GetURI(), _mutex,
		 spec.GetAudioFormat().TimeToSize(DEFAULT_BUFFER_TIME),
		 spec.GetAudioFormat().TimeToSize(DEFAULT_RESUME_TIME)),
	 MultiSocketMonitor(_loop),
	 device(spec.GetDeviceName()),
	 frame_size(spec.GetAudioFormat().GetFrameSize()),
	 defer_invalidate_sockets(_loop,
				  BIND_THIS_METHOD(InvalidateSockets))
{
	OpenDevice(spec.GetAudioFormat());

	SetMimeType(fmt::format("audio/x-mpd-alsa-pcm;format={}",
				spec.GetFormatString()));

	InputStream::SetReady();

	snd_pcm_start(capture_handle);

	defer_invalidate_sockets.Schedule();
}

inline InputStreamPtr
AlsaInputStream::Create(EventLoop &event_loop, std::string_view uri,
			Mutex &mutex)
{
	AlsaInputStream::SourceSpec spec(uri);
	if (!spec.IsValidScheme())
		return nullptr;

	return std::make_unique<AlsaInputStream>(event_loop, mutex, spec);
}

Event::Duration
AlsaInputStream::PrepareSockets() noexcept
{
	if (IsPaused()) {
		ClearSocketList();
		return Event::Duration(-1);
	}

	return non_block.PrepareSockets(*this, capture_handle);
}

void
AlsaInputStream::DispatchSockets() noexcept
try {
	non_block.DispatchSockets(*this, capture_handle);

	const std::scoped_lock protect{mutex};

	auto w = PrepareWriteBuffer();
	const snd_pcm_uframes_t w_frames = w.size() / frame_size;
	if (w_frames == 0) {
		/* buffer is full */
		Pause();
		return;
	}

	snd_pcm_sframes_t n_frames;
	while ((n_frames = snd_pcm_readi(capture_handle,
					 w.data(), w_frames)) < 0) {
		if (n_frames == -EAGAIN)
			return;

		if (Recover(n_frames) < 0)
			throw std::runtime_error("PCM error - stream aborted");
	}

	size_t nbytes = n_frames * frame_size;
	CommitWriteBuffer(nbytes);
}
catch (...) {
	postponed_exception = std::current_exception();
	InvokeOnAvailable();
}

inline int
AlsaInputStream::Recover(int err)
{
	switch(err) {
	case -EPIPE:
		FmtDebug(alsa_input_domain,
			 "Overrun on ALSA capture device {:?}",
			 device);
		break;

	case -ESTRPIPE:
		FmtDebug(alsa_input_domain,
			 "ALSA capture device {:?} was suspended",
			 device);
		break;
	}

	switch (snd_pcm_state(capture_handle)) {
	case SND_PCM_STATE_PAUSED:
		err = snd_pcm_pause(capture_handle, /* disable */ 0);
		break;

	case SND_PCM_STATE_SUSPENDED:
		err = snd_pcm_resume(capture_handle);
		if (err == -EAGAIN)
			return 0;
		/* fall-through to snd_pcm_prepare: */
		[[fallthrough]];

	case SND_PCM_STATE_OPEN:
	case SND_PCM_STATE_SETUP:
	case SND_PCM_STATE_XRUN:
		err = snd_pcm_prepare(capture_handle);
		if (err == 0)
			err = snd_pcm_start(capture_handle);
		break;

	case SND_PCM_STATE_DISCONNECTED:
		break;

	case SND_PCM_STATE_PREPARED:
	case SND_PCM_STATE_RUNNING:
	case SND_PCM_STATE_DRAINING:
		/* this is no error, so just keep running */
		err = 0;
		break;

	default:
		/* this default case is just here to work around
		   -Wswitch due to SND_PCM_STATE_PRIVATE1 (libasound
		   1.1.6) */
		break;
	}

	return err;
}

void
AlsaInputStream::ConfigureCapture(AudioFormat audio_format)
{
	int err;

	snd_pcm_hw_params_t *hw_params;
	snd_pcm_hw_params_alloca(&hw_params);

	if ((err = snd_pcm_hw_params_any(capture_handle, hw_params)) < 0)
		throw Alsa::MakeError(err, "snd_pcm_hw_params_any() failed");

	if ((err = snd_pcm_hw_params_set_access(capture_handle, hw_params,
	                                       SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
		throw Alsa::MakeError(err, "snd_pcm_hw_params_set_access() failed");

	if ((err = snd_pcm_hw_params_set_format(capture_handle, hw_params,
	                               	ToAlsaPcmFormat(audio_format.format))) < 0)
		throw Alsa::MakeError(err, "Cannot set sample format");

	if ((err = snd_pcm_hw_params_set_channels(capture_handle,
	                                    hw_params, audio_format.channels)) < 0)
		throw Alsa::MakeError(err, "Cannot set channels");

	if ((err = snd_pcm_hw_params_set_rate(capture_handle,
	                              hw_params, audio_format.sample_rate, 0)) < 0)
		throw Alsa::MakeError(err, "Cannot set sample rate");

	snd_pcm_uframes_t buffer_size_min, buffer_size_max;
	snd_pcm_hw_params_get_buffer_size_min(hw_params, &buffer_size_min);
	snd_pcm_hw_params_get_buffer_size_max(hw_params, &buffer_size_max);
	unsigned buffer_time_min, buffer_time_max;
	snd_pcm_hw_params_get_buffer_time_min(hw_params, &buffer_time_min, nullptr);
	snd_pcm_hw_params_get_buffer_time_max(hw_params, &buffer_time_max, nullptr);
	FmtDebug(alsa_input_domain, "buffer: size={}..{} time={}..{}",
		 buffer_size_min, buffer_size_max,
		 buffer_time_min, buffer_time_max);

	snd_pcm_uframes_t period_size_min, period_size_max;
	snd_pcm_hw_params_get_period_size_min(hw_params, &period_size_min, nullptr);
	snd_pcm_hw_params_get_period_size_max(hw_params, &period_size_max, nullptr);
	unsigned period_time_min, period_time_max;
	snd_pcm_hw_params_get_period_time_min(hw_params, &period_time_min, nullptr);
	snd_pcm_hw_params_get_period_time_max(hw_params, &period_time_max, nullptr);
	FmtDebug(alsa_input_domain, "period: size={}..{} time={}..{}",
		 period_size_min, period_size_max,
		 period_time_min, period_time_max);

	/* choose the maximum buffer_time up to limit of 2 seconds ... */
	unsigned buffer_time = buffer_time_max;
	if (buffer_time > 2000000U)
		buffer_time = 2000000U;
	int direction = -1;
	if ((err = snd_pcm_hw_params_set_buffer_time_near(capture_handle,
				hw_params, &buffer_time, &direction)) < 0)
		throw Alsa::MakeError(err, "Cannot set buffer time");

	/* ... and calculate the period_size to have four periods in
	   one buffer; this way, we get woken up often enough to avoid
	   buffer overruns, but not too often */
	snd_pcm_uframes_t buffer_size;
	if (snd_pcm_hw_params_get_buffer_size(hw_params, &buffer_size) == 0) {
		snd_pcm_uframes_t period_size = buffer_size / 4;
		direction = -1;
		if ((err = snd_pcm_hw_params_set_period_size_near(capture_handle,
		                             hw_params, &period_size, &direction)) < 0)
			throw Alsa::MakeError(err, "Cannot set period size");
	}

	if ((err = snd_pcm_hw_params(capture_handle, hw_params)) < 0)
		throw Alsa::MakeError(err, "snd_pcm_hw_params() failed");

	snd_pcm_uframes_t alsa_buffer_size;
	err = snd_pcm_hw_params_get_buffer_size(hw_params, &alsa_buffer_size);
	if (err < 0)
		throw Alsa::MakeError(err, "snd_pcm_hw_params_get_buffer_size() failed");

	snd_pcm_uframes_t alsa_period_size;
	err = snd_pcm_hw_params_get_period_size(hw_params, &alsa_period_size,
						nullptr);
	if (err < 0)
		throw Alsa::MakeError(err, "snd_pcm_hw_params_get_period_size() failed");

	FmtDebug(alsa_input_domain, "buffer_size={} period_size={}",
		 alsa_buffer_size, alsa_period_size);

	snd_pcm_sw_params_t *sw_params;
	snd_pcm_sw_params_alloca(&sw_params);

	snd_pcm_sw_params_current(capture_handle, sw_params);

	if ((err = snd_pcm_sw_params(capture_handle, sw_params)) < 0)
		throw Alsa::MakeError(err, "snd_pcm_sw_params() failed");
}

inline void
AlsaInputStream::OpenDevice(const AudioFormat &audio_format)
{
	int err;

	if ((err = snd_pcm_open(&capture_handle, device.c_str(),
				SND_PCM_STREAM_CAPTURE,
				SND_PCM_NONBLOCK | global_config.mode)) < 0)
		throw Alsa::MakeError(err,
				      FmtBuffer<256>("Failed to open device {}",
						     device));

	try {
		ConfigureCapture(audio_format);
	} catch (...) {
		snd_pcm_close(capture_handle);
		throw;
	}

	snd_pcm_prepare(capture_handle);
}

/*#########################  Plugin Functions  ##############################*/


static void
alsa_input_init(EventLoop &event_loop, const ConfigBlock &block)
{
	global_config.event_loop = &event_loop;
	global_config.default_device = block.GetBlockValue("default_device", BUILTIN_DEFAULT_DEVICE);
	global_config.default_format = block.GetBlockValue("default_format", BUILTIN_DEFAULT_FORMAT);
	global_config.mode = 0;

#ifdef SND_PCM_NO_AUTO_RESAMPLE
	if (!block.GetBlockValue("auto_resample", true))
		global_config.mode |= SND_PCM_NO_AUTO_RESAMPLE;
#endif

#ifdef SND_PCM_NO_AUTO_CHANNELS
	if (!block.GetBlockValue("auto_channels", true))
		global_config.mode |= SND_PCM_NO_AUTO_CHANNELS;
#endif

#ifdef SND_PCM_NO_AUTO_FORMAT
	if (!block.GetBlockValue("auto_format", true))
		global_config.mode |= SND_PCM_NO_AUTO_FORMAT;
#endif
}

static InputStreamPtr
alsa_input_open(std::string_view uri, Mutex &mutex)
{
	return AlsaInputStream::Create(*global_config.event_loop, uri,
				       mutex);
}

static constexpr const char *alsa_prefixes[] = {
	ALSA_URI_PREFIX,
	nullptr
};

const struct InputPlugin input_plugin_alsa = {
	"alsa",
	alsa_prefixes,
	alsa_input_init,
	nullptr,
	alsa_input_open,
	nullptr
};
