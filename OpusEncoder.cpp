//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#include "OpusEncoder.h"
#include <assert.h>
#include "logging.h"
#include "VoIPServerConfig.h"

tgvoip::OpusEncoder::OpusEncoder(MediaStreamItf
                                 *source): queue(11), bufferPool(960 * 2, 10)
{
	this->source = source;
	source->SetCallback(tgvoip::OpusEncoder::Callback,
	                    this);
	enc = opus_encoder_create(48000, 1,
	                          OPUS_APPLICATION_VOIP, NULL);
	opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(10));
	opus_encoder_ctl(enc,
	                 OPUS_SET_PACKET_LOSS_PERC(15));
	opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
	opus_encoder_ctl(enc,
	                 OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
	opus_encoder_ctl(enc,
	                 OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
	requestedBitrate = 32000;
	currentBitrate = 0;
	running = false;
	echoCanceller = NULL;
	complexity = 10;
	frameDuration = 20;
	mediumCorrectionBitrate =
	    ServerConfig::GetSharedInstance()->GetInt("audio_medium_fec_bitrate",
	            10000);
	strongCorrectionBitrate =
	    ServerConfig::GetSharedInstance()->GetInt("audio_strong_fec_bitrate",
	            8000);
	mediumCorrectionMultiplier =
	    ServerConfig::GetSharedInstance()->GetDouble("audio_medium_fec_multiplier",
	            1.5);
	strongCorrectionMultiplier =
	    ServerConfig::GetSharedInstance()->GetDouble("audio_strong_fec_multiplier",
	            2.0);
}

tgvoip::OpusEncoder::~OpusEncoder()
{
	opus_encoder_destroy(enc);
}

void tgvoip::OpusEncoder::Start()
{
	if(running) {
		return;
	}
	running = true;
	start_thread(thread, StartThread, this);
	set_thread_priority(thread,
	                    get_thread_max_priority());
	set_thread_name(thread, "opus_encoder");
}

void tgvoip::OpusEncoder::Stop()
{
	if(!running) {
		return;
	}
	running = false;
	queue.Put(NULL);
	join_thread(thread);
}


void tgvoip::OpusEncoder::SetBitrate(
    uint32_t bitrate)
{
	requestedBitrate = bitrate;
}

void tgvoip::OpusEncoder::Encode(unsigned char
                                 *data, size_t len)
{
	if(requestedBitrate != currentBitrate) {
		opus_encoder_ctl(enc,
		                 OPUS_SET_BITRATE(requestedBitrate));
		currentBitrate = requestedBitrate;
		LOGV("opus_encoder: setting bitrate to %u",
		     currentBitrate);
	}
	int32_t r = opus_encode(enc, (int16_t *)data, len / 2,
	                        buffer, 4096);
	if(r <= 0) {
		LOGE("Error encoding: %d", r);
	} else if(r == 1) {
		LOGW("DTX");
	} else if(running) {
		//LOGV("Packet size = %d", r);
		InvokeCallback(buffer, (size_t)r);
	}
}

size_t tgvoip::OpusEncoder::Callback(
    unsigned char *data, size_t len, void *param)
{
	OpusEncoder *e = (OpusEncoder *)param;
	unsigned char *buf = e->bufferPool.Get();
	if(buf) {
		assert(len == 960 * 2);
		memcpy(buf, data, 960 * 2);
		e->queue.Put(buf);
	} else {
		LOGW("opus_encoder: no buffer slots left");
		if(e->complexity > 1) {
			e->complexity--;
			opus_encoder_ctl(e->enc,
			                 OPUS_SET_COMPLEXITY(e->complexity));
		}
	}
	return 0;
}


uint32_t tgvoip::OpusEncoder::GetBitrate()
{
	return requestedBitrate;
}

void tgvoip::OpusEncoder::SetEchoCanceller(
    EchoCanceller *aec)
{
	echoCanceller = aec;
}

void *tgvoip::OpusEncoder::StartThread(void *arg)
{
	((OpusEncoder *)arg)->RunThread();
	return NULL;
}

void tgvoip::OpusEncoder::RunThread()
{
	unsigned char buf[960 * 2];
	uint32_t bufferedCount = 0;
	uint32_t packetsPerFrame = frameDuration / 20;
	LOGV("starting decoder, packets per frame=%d",
	     packetsPerFrame);
	unsigned char *frame;
	if(packetsPerFrame > 1) {
		frame = (unsigned char *) malloc(
		            960 * 2 * packetsPerFrame);
	} else {
		frame = NULL;
	}
	while(running) {
		unsigned char *packet = (unsigned char *)
		                        queue.GetBlocking();
		if(packet) {
			if(echoCanceller) {
				echoCanceller->ProcessInput(packet, buf, 960 * 2);
			} else {
				memcpy(buf, packet, 960 * 2);
			}
			if(packetsPerFrame == 1) {
				Encode(buf, 960 * 2);
			} else {
				memcpy(frame + (960 * 2 * bufferedCount), buf, 960 * 2);
				bufferedCount++;
				if(bufferedCount == packetsPerFrame) {
					Encode(frame, 960 * 2 * packetsPerFrame);
					bufferedCount = 0;
				}
			}
			bufferPool.Reuse(packet);
		}
	}
	if(frame) {
		free(frame);
	}
}


void tgvoip::OpusEncoder::SetOutputFrameDuration(
    uint32_t duration)
{
	frameDuration = duration;
}


void tgvoip::OpusEncoder::SetPacketLoss(
    int percent)
{
	packetLossPercent = percent;
	double multiplier = 1;
	if(currentBitrate <= strongCorrectionBitrate) {
		multiplier = strongCorrectionMultiplier;
	} else if(currentBitrate <=
	          mediumCorrectionBitrate) {
		multiplier = mediumCorrectionMultiplier;
	}
	opus_encoder_ctl(enc,
	                 OPUS_SET_PACKET_LOSS_PERC((int)(
	                             percent * multiplier)));
	opus_encoder_ctl(enc,
	                 OPUS_SET_BANDWIDTH(percent > 17 ? OPUS_AUTO :
	                                    OPUS_BANDWIDTH_FULLBAND));
}

int tgvoip::OpusEncoder::GetPacketLoss()
{
	return packetLossPercent;
}
