//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#ifndef LIBTGVOIP_AUDIOOUTPUTAUDIOUNIT_H
#define LIBTGVOIP_AUDIOOUTPUTAUDIOUNIT_H

#include <AudioUnit/AudioUnit.h>
#include "../../audio/AudioOutput.h"

namespace tgvoip
{
namespace audio
{
class AudioUnitIO;

class AudioOutputAudioUnit : public AudioOutput {
public:
	AudioOutputAudioUnit();
	virtual ~AudioOutputAudioUnit();
	virtual void Configure(uint32_t sampleRate,
	                       uint32_t bitsPerSample, uint32_t channels);
	virtual bool IsPhone();
	virtual void EnableLoudspeaker(bool enabled);
	virtual void Start();
	virtual void Stop();
	virtual bool IsPlaying();
	virtual float GetLevel();
	void HandleBufferCallback(AudioBufferList *
	                          ioData);

private:
	bool isPlaying;
	unsigned char remainingData[10240];
	size_t remainingDataSize;
	AudioUnitIO *io;
	float level;
	int16_t absMax;
	int count;
};
}
}

#endif //LIBTGVOIP_AUDIOOUTPUTAUDIOUNIT_H
