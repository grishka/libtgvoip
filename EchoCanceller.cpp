//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#include "EchoCanceller.h"
#include "audio/AudioOutput.h"
#include "audio/AudioInput.h"
#include "logging.h"
#include <string.h>
#include <stdio.h>

#ifndef TGVOIP_NO_DSP
#ifndef TGVOIP_USE_DESKTOP_DSP
#include "webrtc/modules/audio_processing/aecm/echo_control_mobile.h"
#include "webrtc/modules/audio_processing/ns/noise_suppression_x.h"
#else
#include "webrtc/modules/audio_processing/aec/echo_cancellation.h"
//#include "webrtc/modules/audio_processing/ns/noise_suppression.h"
#include "webrtc/modules/audio_processing/ns/noise_suppression_x.h"
#endif
#include "webrtc/modules/audio_processing/splitting_filter.h"
#include "webrtc/common_audio/channel_buffer.h"
#include "webrtc/modules/audio_processing/agc/legacy/gain_control.h"
#endif

#define AEC_FRAME_SIZE 160
#define OFFSET_STEP AEC_FRAME_SIZE*2

//#define CLAMP(x, min, max) (x<max ? (x>min ? x : min) : max)
#define CLAMP(x, min, max) x

using namespace tgvoip;

#ifdef TGVOIP_USE_DESKTOP_DSP
namespace webrtc
{
void WebRtcAec_enable_delay_agnostic(
    AecCore *self, int enable);
}
#endif

EchoCanceller::EchoCanceller(bool enableAEC,
                             bool enableNS, bool enableAGC)
{
	this->enableAEC = enableAEC;
	this->enableAGC = enableAGC;
	this->enableNS = enableNS;

#ifndef TGVOIP_NO_DSP

	splittingFilter = new webrtc::SplittingFilter(1, 3,
	        960);
	splittingFilterFarend = new webrtc::SplittingFilter(
	    1, 3, 960);

	splittingFilterIn = new webrtc::IFChannelBuffer(960,
	        1, 1);
	splittingFilterFarendIn = new
	webrtc::IFChannelBuffer(960, 1, 1);
	splittingFilterOut = new webrtc::IFChannelBuffer(
	    960, 1, 3);
	splittingFilterFarendOut = new
	webrtc::IFChannelBuffer(960, 1, 3);

	if(enableAEC) {
		init_mutex(aecMutex);
#ifndef TGVOIP_USE_DESKTOP_DSP
		aec = WebRtcAecm_Create();
		WebRtcAecm_Init(aec, 16000);
		AecmConfig cfg;
		cfg.cngMode = AecmFalse;
		cfg.echoMode = 1;
		WebRtcAecm_set_config(aec, cfg);
#else
		aec = webrtc::WebRtcAec_Create();
		webrtc::WebRtcAec_Init(aec, 48000, 48000);
		webrtc::WebRtcAec_enable_delay_agnostic(
		    webrtc::WebRtcAec_aec_core(aec), 1);
		webrtc::AecConfig config;
		config.metricsMode = webrtc::kAecFalse;
		config.nlpMode = webrtc::kAecNlpAggressive;
		config.skewMode = webrtc::kAecFalse;
		config.delay_logging = webrtc::kAecFalse;
		webrtc::WebRtcAec_set_config(aec, config);
#endif

		farendQueue = new BlockingQueue(11);
		farendBufferPool = new BufferPool(960 * 2, 10);
		running = true;

		start_thread(bufferFarendThread,
		             EchoCanceller::StartBufferFarendThread, this);
	}

	if(enableNS) {
//#ifndef TGVOIP_USE_DESKTOP_DSP
		ns = WebRtcNsx_Create();
		WebRtcNsx_Init((NsxHandle *)ns, 48000);
		WebRtcNsx_set_policy((NsxHandle *)ns, 2);
		/*#else
				ns=WebRtcNs_Create();
				WebRtcNs_Init((NsHandle*)ns, 48000);
				WebRtcNs_set_policy((NsHandle*)ns, 1);
		#endif*/
	}

	if(enableAGC) {
		agc = WebRtcAgc_Create();
		WebRtcAgcConfig agcConfig;
		agcConfig.compressionGaindB = 9;
		agcConfig.limiterEnable = 1;
		agcConfig.targetLevelDbfs = 3;
		WebRtcAgc_Init(agc, 0, 255,
		               kAgcModeAdaptiveAnalog, 48000);
		WebRtcAgc_set_config(agc, agcConfig);
		agcMicLevel = 128;
	}

	/*state=webrtc::WebRtcAec_Create();
	webrtc::WebRtcAec_Init(state, 16000, 16000);
	webrtc::WebRtcAec_enable_delay_agnostic(webrtc::WebRtcAec_aec_core(state), 1);*/
#endif
}

EchoCanceller::~EchoCanceller()
{
	if(enableAEC) {
		running = false;
		farendQueue->Put(NULL);
		join_thread(bufferFarendThread);
		delete farendQueue;
		delete farendBufferPool;
#ifndef TGVOIP_USE_DESKTOP_DSP
		WebRtcAecm_Free(aec);
#else
		webrtc::WebRtcAec_Free(aec);
#endif
	}
	if(enableNS) {
//#ifndef TGVOIP_USE_DESKTOP_DSP
		WebRtcNsx_Free((NsxHandle *)ns);
		/*#else
				WebRtcNs_Free((NsHandle*)ns);
		#endif*/
	}
	if(enableAGC) {
		WebRtcAgc_Free(agc);
	}
	//webrtc::WebRtcAec_Free(state);

	delete (webrtc::SplittingFilter *)splittingFilter;
	delete (webrtc::SplittingFilter *)
	splittingFilterFarend;

	delete (webrtc::IFChannelBuffer *)
	splittingFilterIn;
	delete (webrtc::IFChannelBuffer *)
	splittingFilterOut;
	delete (webrtc::IFChannelBuffer *)
	splittingFilterFarendIn;
	delete (webrtc::IFChannelBuffer *)
	splittingFilterFarendOut;

	if (this->enableAEC) {
		free_mutex(aecMutex);
	}
}

void EchoCanceller::Start()
{

}

void EchoCanceller::Stop()
{

}


void EchoCanceller::SpeakerOutCallback(
    unsigned char *data, size_t len)
{
	if(len != 960 * 2 || !enableAEC) {
		return;
	}
	/*size_t offset=0;
	while(offset<len){
		WebRtcAecm_BufferFarend(state, (int16_t*)(data+offset), AEC_FRAME_SIZE);
		offset+=OFFSET_STEP;
	}*/
	unsigned char *buf = farendBufferPool->Get();
	if(buf) {
		memcpy(buf, data, 960 * 2);
		farendQueue->Put(buf);
	}
}

void *EchoCanceller::StartBufferFarendThread(
    void *arg)
{
	((EchoCanceller *)arg)->RunBufferFarendThread();
	return NULL;
}

void EchoCanceller::RunBufferFarendThread()
{
	while(running) {
		int16_t *samplesIn = (int16_t *)
		                     farendQueue->GetBlocking();
		if(samplesIn) {
			webrtc::IFChannelBuffer *bufIn =
			    (webrtc::IFChannelBuffer *)
			    splittingFilterFarendIn;
			webrtc::IFChannelBuffer *bufOut =
			    (webrtc::IFChannelBuffer *)
			    splittingFilterFarendOut;
			memcpy(bufIn->ibuf()->bands(0)[0], samplesIn,
			       960 * 2);
			farendBufferPool->Reuse((unsigned char *)
			                        samplesIn);
			((webrtc::SplittingFilter *)
			 splittingFilterFarend)->Analysis(bufIn, bufOut);
			lock_mutex(aecMutex);
#ifndef TGVOIP_USE_DESKTOP_DSP
			WebRtcAecm_BufferFarend(aec,
			                        bufOut->ibuf_const()->bands(0)[0], 160);
			WebRtcAecm_BufferFarend(aec,
			                        bufOut->ibuf_const()->bands(0)[0] + 160, 160);
#else
			webrtc::WebRtcAec_BufferFarend(aec,
			                               bufOut->fbuf_const()->bands(0)[0], 160);
			webrtc::WebRtcAec_BufferFarend(aec,
			                               bufOut->fbuf_const()->bands(0)[0] + 160, 160);
#endif
			unlock_mutex(aecMutex);
			didBufferFarend = true;
		}
	}
}

void EchoCanceller::Enable(bool enabled)
{
	//isOn=enabled;
}

void EchoCanceller::ProcessInput(unsigned char *
                                 data, unsigned char *out, size_t len)
{
	int i;
	if(!enableAEC && !enableAGC && !enableNS) {
		memcpy(out, data, len);
		return;
	}
	int16_t *samplesIn = (int16_t *)data;
	int16_t *samplesOut = (int16_t *)out;

	webrtc::IFChannelBuffer *bufIn =
	    (webrtc::IFChannelBuffer *) splittingFilterIn;
	webrtc::IFChannelBuffer *bufOut =
	    (webrtc::IFChannelBuffer *) splittingFilterOut;

	memcpy(bufIn->ibuf()->bands(0)[0], samplesIn,
	       960 * 2);

	((webrtc::SplittingFilter *)
	 splittingFilter)->Analysis(bufIn, bufOut);

	if(enableAGC) {
		int16_t _agcOut[3][320];
		int16_t *agcIn[3];
		int16_t *agcOut[3];
		for(i = 0; i < 3; i++) {
			agcIn[i] = (int16_t *)bufOut->ibuf_const()->bands(
			               0)[i];
			agcOut[i] = _agcOut[i];
		}
		uint8_t saturation;
		WebRtcAgc_AddMic(agc, agcIn, 3, 160);
		WebRtcAgc_Process(agc,
		                  (const int16_t *const *) agcIn, 3, 160, agcOut,
		                  agcMicLevel, &agcMicLevel, 0, &saturation);
		for(i = 0; i < 3; i++) {
			agcOut[i] += 160;
			agcIn[i] += 160;
		}
		WebRtcAgc_AddMic(agc, agcIn, 3, 160);
		WebRtcAgc_Process(agc,
		                  (const int16_t *const *) agcIn, 3, 160, agcOut,
		                  agcMicLevel, &agcMicLevel, 0, &saturation);
		//LOGV("AGC mic level %d", agcMicLevel);
		memcpy(bufOut->ibuf()->bands(0)[0], _agcOut[0],
		       320 * 2);
		memcpy(bufOut->ibuf()->bands(0)[1], _agcOut[1],
		       320 * 2);
		memcpy(bufOut->ibuf()->bands(0)[2], _agcOut[2],
		       320 * 2);
	}

#ifndef TGVOIP_USE_DESKTOP_DSP
	if(enableAEC && enableNS) {
		int16_t _nsOut[3][320];
		int16_t *nsIn[3];
		int16_t *nsOut[3];
		for(i = 0; i < 3; i++) {
			nsIn[i] = (int16_t *)bufOut->ibuf_const()->bands(
			              0)[i];
			nsOut[i] = _nsOut[i];
		}
		WebRtcNsx_Process((NsxHandle *)ns,
		                  (const short * const *) nsIn, 3, nsOut);
		for(i = 0; i < 3; i++) {
			nsOut[i] += 160;
			nsIn[i] += 160;
		}
		WebRtcNsx_Process((NsxHandle *)ns,
		                  (const short * const *) nsIn, 3, nsOut);

		memcpy(bufOut->ibuf()->bands(0)[1], _nsOut[1],
		       320 * 2 * 2);

		lock_mutex(aecMutex);
		WebRtcAecm_Process(aec,
		                   bufOut->ibuf()->bands(0)[0], _nsOut[0],
		                   samplesOut, AEC_FRAME_SIZE,
		                   (int16_t) tgvoip::audio::AudioOutput::GetEstimatedDelay());
		WebRtcAecm_Process(aec,
		                   bufOut->ibuf()->bands(0)[0] + 160, _nsOut[0] + 160,
		                   samplesOut + 160, AEC_FRAME_SIZE,
		                   (int16_t) tgvoip::audio::AudioOutput::GetEstimatedDelay()
		                   + audio::AudioInput::GetEstimatedDelay());
		unlock_mutex(aecMutex);
		memcpy(bufOut->ibuf()->bands(0)[0], samplesOut,
		       320 * 2);
	} else if(enableAEC) {
		lock_mutex(aecMutex);
		WebRtcAecm_Process(aec,
		                   bufOut->ibuf()->bands(0)[0], NULL, samplesOut,
		                   AEC_FRAME_SIZE,
		                   (int16_t) tgvoip::audio::AudioOutput::GetEstimatedDelay());
		WebRtcAecm_Process(aec,
		                   bufOut->ibuf()->bands(0)[0] + 160, NULL,
		                   samplesOut + 160, AEC_FRAME_SIZE,
		                   (int16_t) tgvoip::audio::AudioOutput::GetEstimatedDelay()
		                   + audio::AudioInput::GetEstimatedDelay());
		unlock_mutex(aecMutex);
		memcpy(bufOut->ibuf()->bands(0)[0], samplesOut,
		       320 * 2);
	} else if(enableNS) {
		int16_t _nsOut[3][320];
		int16_t *nsIn[3];
		int16_t *nsOut[3];
		for(i = 0; i < 3; i++) {
			nsIn[i] = (int16_t *)bufOut->ibuf_const()->bands(
			              0)[i];
			nsOut[i] = _nsOut[i];
		}
		WebRtcNsx_Process((NsxHandle *)ns,
		                  (const short * const *) nsIn, 3, nsOut);
		for(i = 0; i < 3; i++) {
			nsOut[i] += 160;
			nsIn[i] += 160;
		}
		WebRtcNsx_Process((NsxHandle *)ns,
		                  (const short * const *) nsIn, 3, nsOut);

		memcpy(bufOut->ibuf()->bands(0)[0], _nsOut[0],
		       320 * 2);
		memcpy(bufOut->ibuf()->bands(0)[1], _nsOut[1],
		       320 * 2);
		memcpy(bufOut->ibuf()->bands(0)[2], _nsOut[2],
		       320 * 2);
	}
#else
	/*if(enableNS){
		float _nsOut[3][320];
		const float* nsIn[3];
		float* nsOut[3];
		for(i=0;i<3;i++){
			nsIn[i]=bufOut->fbuf_const()->bands(0)[i];
			nsOut[i]=_nsOut[i];
		}
		WebRtcNs_Process((NsHandle*)ns, nsIn, 3, nsOut);
		for(i=0;i<3;i++){
			nsOut[i]+=160;
			nsIn[i]+=160;
		}
		WebRtcNs_Process((NsHandle*)ns, nsIn, 3, nsOut);

		memcpy(bufOut->fbuf()->bands(0)[0], _nsOut[0], 320*4);
		memcpy(bufOut->fbuf()->bands(0)[1], _nsOut[1], 320*4);
		memcpy(bufOut->fbuf()->bands(0)[2], _nsOut[2], 320*4);
	}*/
	if(enableNS) {
		int16_t _nsOut[3][320];
		int16_t *nsIn[3];
		int16_t *nsOut[3];
		for(i = 0; i < 3; i++) {
			nsIn[i] = (int16_t *)bufOut->ibuf_const()->bands(
			              0)[i];
			nsOut[i] = _nsOut[i];
		}
		WebRtcNsx_Process((NsxHandle *)ns,
		                  (const short * const *)nsIn, 3, nsOut);
		for(i = 0; i < 3; i++) {
			nsOut[i] += 160;
			nsIn[i] += 160;
		}
		WebRtcNsx_Process((NsxHandle *)ns,
		                  (const short * const *)nsIn, 3, nsOut);

		memcpy(bufOut->ibuf()->bands(0)[0], _nsOut[0],
		       320 * 2);
		memcpy(bufOut->ibuf()->bands(0)[1], _nsOut[1],
		       320 * 2);
		memcpy(bufOut->ibuf()->bands(0)[2], _nsOut[2],
		       320 * 2);
	}

	if(enableAEC) {
		const float *aecIn[3];
		float *aecOut[3];
		float _aecOut[3][320];
		for(i = 0; i < 3; i++) {
			aecIn[i] = bufOut->fbuf_const()->bands(0)[i];
			aecOut[i] = _aecOut[i];
		}
		webrtc::WebRtcAec_Process(aec, aecIn, 3, aecOut,
		                          AEC_FRAME_SIZE,
		                          audio::AudioOutput::GetEstimatedDelay()
		                          + audio::AudioInput::GetEstimatedDelay(), 0);
		for(i = 0; i < 3; i++) {
			aecOut[i] += 160;
			aecIn[i] += 160;
		}
		webrtc::WebRtcAec_Process(aec, aecIn, 3, aecOut,
		                          AEC_FRAME_SIZE,
		                          audio::AudioOutput::GetEstimatedDelay()
		                          + audio::AudioInput::GetEstimatedDelay(), 0);

		memcpy(bufOut->fbuf()->bands(0)[0], _aecOut[0],
		       320 * 4);
		memcpy(bufOut->fbuf()->bands(0)[1], _aecOut[1],
		       320 * 4);
		memcpy(bufOut->fbuf()->bands(0)[2], _aecOut[2],
		       320 * 4);
	}
#endif

	((webrtc::SplittingFilter *)
	 splittingFilter)->Synthesis(bufOut, bufIn);

	memcpy(samplesOut,
	       bufIn->ibuf_const()->bands(0)[0], 960 * 2);
}

