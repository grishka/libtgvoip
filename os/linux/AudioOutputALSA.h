//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#ifndef LIBTGVOIP_AUDIOOUTPUTALSA_H
#define LIBTGVOIP_AUDIOOUTPUTALSA_H

#include "../../audio/AudioOutput.h"
#include "../../threading.h"
#include <alsa/asoundlib.h>

namespace tgvoip
{
namespace audio
{

class AudioOutputALSA : public AudioOutput {
public:
	AudioOutputALSA(std::string devID);
	virtual ~AudioOutputALSA();
	virtual void Configure(uint32_t sampleRate,
	                       uint32_t bitsPerSample, uint32_t channels);
	virtual void Start();
	virtual void Stop();
	virtual bool IsPlaying();
	virtual void SetCurrentDevice(std::string devID);
	static void EnumerateDevices(
	    std::vector<AudioOutputDevice> &devs);

private:
	static void *StartThread(void *arg);
	void RunThread();

	int (*_snd_pcm_open)(snd_pcm_t **pcm,
	                     const char *name, snd_pcm_stream_t stream,
	                     int mode);
	int (*_snd_pcm_set_params)(snd_pcm_t *pcm,
	                           snd_pcm_format_t format, snd_pcm_access_t access,
	                           unsigned int channels, unsigned int rate,
	                           int soft_resample, unsigned int latency);
	int (*_snd_pcm_close)(snd_pcm_t *pcm);
	snd_pcm_sframes_t (*_snd_pcm_writei)(
	    snd_pcm_t *pcm, const void *buffer,
	    snd_pcm_uframes_t size);
	int (*_snd_pcm_recover)(snd_pcm_t *pcm, int err,
	                        int silent);
	const char *(*_snd_strerror)(int errnum);
	void *lib;

	snd_pcm_t *handle;
	tgvoip_thread_t thread;
	bool isPlaying;
};

}
}

#endif //LIBTGVOIP_AUDIOOUTPUTALSA_H
