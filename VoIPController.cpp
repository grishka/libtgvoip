//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#ifndef _WIN32
#include <unistd.h>
#include <sys/time.h>
#endif
#include <errno.h>
#include <string.h>
#include <wchar.h>
#include "VoIPController.h"
#include "logging.h"
#include "threading.h"
#include "BufferOutputStream.h"
#include "BufferInputStream.h"
#include "OpusEncoder.h"
#include "OpusDecoder.h"
#include "VoIPServerConfig.h"
#include <assert.h>
#include <time.h>
#include <math.h>
#include <exception>
#include <stdexcept>

using namespace tgvoip;

#ifdef __APPLE__
#include "os/darwin/AudioUnitIO.h"
#include <mach/mach_time.h>
double VoIPController::machTimebase = 0;
uint64_t VoIPController::machTimestart = 0;
#endif

#ifdef _WIN32
int64_t VoIPController::win32TimeScale = 0;
bool VoIPController::didInitWin32TimeScale =
    false;
#endif

#define SHA1_LENGTH 20
#define SHA256_LENGTH 32

#ifndef TGVOIP_USE_CUSTOM_CRYPTO
#include <openssl/sha.h>
#include <openssl/aes.h>
#include <openssl/rand.h>

void tgvoip_openssl_aes_ige_encrypt(uint8_t *in,
                                    uint8_t *out, size_t length, uint8_t *key,
                                    uint8_t *iv)
{
	AES_KEY akey;
	AES_set_encrypt_key(key, 32 * 8, &akey);
	AES_ige_encrypt(in, out, length, &akey, iv,
	                AES_ENCRYPT);
}

void tgvoip_openssl_aes_ige_decrypt(uint8_t *in,
                                    uint8_t *out, size_t length, uint8_t *key,
                                    uint8_t *iv)
{
	AES_KEY akey;
	AES_set_decrypt_key(key, 32 * 8, &akey);
	AES_ige_encrypt(in, out, length, &akey, iv,
	                AES_DECRYPT);
}

void tgvoip_openssl_rand_bytes(uint8_t *buffer,
                               size_t len)
{
	RAND_bytes(buffer, len);
}

void tgvoip_openssl_sha1(uint8_t *msg, size_t len,
                         uint8_t *output)
{
	SHA1(msg, len, output);
}

void tgvoip_openssl_sha256(uint8_t *msg,
                           size_t len, uint8_t *output)
{
	SHA256(msg, len, output);
}

voip_crypto_functions_t VoIPController::crypto = {
	tgvoip_openssl_rand_bytes,
	tgvoip_openssl_sha1,
	tgvoip_openssl_sha256,
	tgvoip_openssl_aes_ige_encrypt,
	tgvoip_openssl_aes_ige_decrypt

};
#else
voip_crypto_functions_t
VoIPController::crypto; // set it yourself upon initialization
#endif

#ifdef _MSC_VER
#define MSC_STACK_FALLBACK(a, b) (b)
#else
#define MSC_STACK_FALLBACK(a, b) (a)
#endif

extern FILE *tgvoipLogFile;

VoIPController::VoIPController() :
	activeNetItfName(""),
	currentAudioInput("default"),
	currentAudioOutput("default")
{
	seq = 1;
	lastRemoteSeq = 0;
	state = STATE_WAIT_INIT;
	audioInput = NULL;
	audioOutput = NULL;
	decoder = NULL;
	encoder = NULL;
	jitterBuffer = NULL;
	audioOutStarted = false;
	audioTimestampIn = 0;
	audioTimestampOut = 0;
	stopping = false;
	int i;
	for(i = 0; i < 20; i++) {
		emptySendBuffers.push_back(new BufferOutputStream(
		                               1024));
	}
	sendQueue = new BlockingQueue(21);
	init_mutex(sendBufferMutex);
	memset(remoteAcks, 0, sizeof(double) * 32);
	memset(sentPacketTimes, 0, sizeof(double) * 32);
	memset(recvPacketTimes, 0, sizeof(double) * 32);
	memset(rttHistory, 0, sizeof(double) * 32);
	memset(sendLossCountHistory, 0,
	       sizeof(uint32_t) * 32);
	memset(&stats, 0, sizeof(voip_stats_t));
	lastRemoteAckSeq = 0;
	lastSentSeq = 0;
	recvLossCount = 0;
	packetsRecieved = 0;
	waitingForAcks = false;
	networkType = NET_TYPE_UNKNOWN;
	audioPacketGrouping = 3;
	audioPacketsWritten = 0;
	currentAudioPacket = NULL;
	stateCallback = NULL;
	echoCanceller = NULL;
	dontSendPackets = 0;
	micMuted = false;
	currentEndpoint = NULL;
	waitingForRelayPeerInfo = false;
	allowP2p = true;
	dataSavingMode = false;
	publicEndpointsReqTime = 0;
	init_mutex(queuedPacketsMutex);
	init_mutex(endpointsMutex);
	connectionInitTime = 0;
	lastRecvPacketTime = 0;
	dataSavingRequestedByPeer = false;
	peerVersion = 0;
	conctl = new CongestionControl();
	prevSendLossCount = 0;
	receivedInit = false;
	receivedInitAck = false;
	peerPreferredRelay = NULL;
	statsDump = NULL;

	socket = NetworkSocket::Create();

	maxAudioBitrate = (uint32_t)
	                  ServerConfig::GetSharedInstance()->GetInt("audio_max_bitrate",
	                          20000);
	maxAudioBitrateGPRS = (uint32_t)
	                      ServerConfig::GetSharedInstance()->GetInt("audio_max_bitrate_gprs",
	                              8000);
	maxAudioBitrateEDGE = (uint32_t)
	                      ServerConfig::GetSharedInstance()->GetInt("audio_max_bitrate_edge",
	                              16000);
	maxAudioBitrateSaving = (uint32_t)
	                        ServerConfig::GetSharedInstance()->GetInt("audio_max_bitrate_saving",
	                                8000);
	initAudioBitrate = (uint32_t)
	                   ServerConfig::GetSharedInstance()->GetInt("audio_init_bitrate",
	                           16000);
	initAudioBitrateGPRS = (uint32_t)
	                       ServerConfig::GetSharedInstance()->GetInt("audio_init_bitrate_gprs",
	                               8000);
	initAudioBitrateEDGE = (uint32_t)
	                       ServerConfig::GetSharedInstance()->GetInt("audio_init_bitrate_edge",
	                               8000);
	initAudioBitrateSaving = (uint32_t)
	                         ServerConfig::GetSharedInstance()->GetInt("audio_init_bitrate_saving",
	                                 8000);
	audioBitrateStepIncr = (uint32_t)
	                       ServerConfig::GetSharedInstance()->GetInt("audio_bitrate_step_incr",
	                               1000);
	audioBitrateStepDecr = (uint32_t)
	                       ServerConfig::GetSharedInstance()->GetInt("audio_bitrate_step_decr",
	                               1000);
	minAudioBitrate = (uint32_t)
	                  ServerConfig::GetSharedInstance()->GetInt("audio_min_bitrate",
	                          8000);
	relaySwitchThreshold =
	    ServerConfig::GetSharedInstance()->GetDouble("relay_switch_threshold",
	            0.8);
	p2pToRelaySwitchThreshold =
	    ServerConfig::GetSharedInstance()->GetDouble("p2p_to_relay_switch_threshold",
	            0.6);
	relayToP2pSwitchThreshold =
	    ServerConfig::GetSharedInstance()->GetDouble("relay_to_p2p_switch_threshold",
	            0.8);

#ifdef __APPLE__
	machTimestart = 0;
#ifdef TGVOIP_USE_AUDIO_SESSION
	needNotifyAcquiredAudioSession = false;
#endif
#endif

	voip_stream_t *stm = (voip_stream_t *) malloc(
	                         sizeof(voip_stream_t));
	stm->id = 1;
	stm->type = STREAM_TYPE_AUDIO;
	stm->codec = CODEC_OPUS;
	stm->enabled = 1;
	stm->frameDuration = 60;
	outgoingStreams.push_back(stm);
}

VoIPController::~VoIPController()
{
	LOGD("Entered VoIPController::~VoIPController");
	if(audioInput) {
		audioInput->Stop();
	}
	if(audioOutput) {
		audioOutput->Stop();
	}
	stopping = true;
	runReceiver = false;
	LOGD("before shutdown socket");
	if(socket) {
		socket->Close();
	}
	sendQueue->Put(NULL);
	LOGD("before join sendThread");
	join_thread(sendThread);
	LOGD("before join recvThread");
	join_thread(recvThread);
	LOGD("before join tickThread");
	join_thread(tickThread);
	free_mutex(sendBufferMutex);
	LOGD("before close socket");
	if(socket) {
		delete socket;
	}
	LOGD("before free send buffers");
	while(emptySendBuffers.size() > 0) {
		delete emptySendBuffers[emptySendBuffers.size()
		                        - 1];
		emptySendBuffers.pop_back();
	}
	while(sendQueue->Size() > 0) {
		void *p = sendQueue->Get();
		if(p) {
			delete (BufferOutputStream *)p;
		}
	}
	LOGD("before delete jitter buffer");
	if(jitterBuffer) {
		delete jitterBuffer;
	}
	LOGD("before stop decoder");
	if(decoder) {
		decoder->Stop();
	}
	LOGD("before delete audio input");
	if(audioInput) {
		delete audioInput;
	}
	LOGD("before delete encoder");
	if(encoder) {
		encoder->Stop();
		delete encoder;
	}
	LOGD("before delete audio output");
	if(audioOutput) {
		delete audioOutput;
	}
	LOGD("before delete decoder");
	if(decoder) {
		delete decoder;
	}
	LOGD("before delete echo canceller");
	if(echoCanceller) {
		echoCanceller->Stop();
		delete echoCanceller;
	}
	delete sendQueue;
	unsigned int i;
	for(i = 0; i < incomingStreams.size(); i++) {
		free(incomingStreams[i]);
	}
	incomingStreams.clear();
	for(i = 0; i < outgoingStreams.size(); i++) {
		free(outgoingStreams[i]);
	}
	outgoingStreams.clear();
	free_mutex(queuedPacketsMutex);
	free_mutex(endpointsMutex);
	for(i = 0; i < queuedPackets.size(); i++) {
		if(queuedPackets[i]->data) {
			free(queuedPackets[i]->data);
		}
		free(queuedPackets[i]);
	}
	delete conctl;
	for(std::vector<Endpoint *>::iterator itr =
	            endpoints.begin(); itr != endpoints.end(); ++itr) {
		delete *itr;
	}
	LOGD("Left VoIPController::~VoIPController");
	if(tgvoipLogFile) {
		FILE *log = tgvoipLogFile;
		tgvoipLogFile = NULL;
		fclose(log);
	}
	if(statsDump) {
		fclose(statsDump);
	}
}

void VoIPController::SetRemoteEndpoints(
    std::vector<Endpoint> endpoints, bool allowP2p)
{
	LOGW("Set remote endpoints");
	preferredRelay = NULL;
	size_t i;
	lock_mutex(endpointsMutex);
	this->endpoints.clear();
	for(std::vector<Endpoint>::iterator itrtr =
	            endpoints.begin(); itrtr != endpoints.end();
	        ++itrtr) {
		this->endpoints.push_back(new Endpoint(*itrtr));
	}
	unlock_mutex(endpointsMutex);
	currentEndpoint = this->endpoints[0];
	preferredRelay = currentEndpoint;
	this->allowP2p = allowP2p;
}

void *VoIPController::StartRecvThread(
    void *controller)
{
	((VoIPController *)controller)->RunRecvThread();
	return NULL;
}

void *VoIPController::StartSendThread(
    void *controller)
{
	((VoIPController *)controller)->RunSendThread();
	return NULL;
}


void *VoIPController::StartTickThread(
    void *controller)
{
	((VoIPController *) controller)->RunTickThread();
	return NULL;
}


void VoIPController::Start()
{
	int res;
	LOGW("Starting voip controller");
	int32_t cfgFrameSize =
	    ServerConfig::GetSharedInstance()->GetInt("audio_frame_size",
	            60);
	if(cfgFrameSize == 20 || cfgFrameSize == 40 ||
	        cfgFrameSize == 60) {
		outgoingStreams[0]->frameDuration = (uint16_t)
		                                    cfgFrameSize;
	}
	socket->Open();


	SendPacket(NULL, 0, currentEndpoint);

	runReceiver = true;
	start_thread(recvThread, StartRecvThread, this);
	set_thread_priority(recvThread,
	                    get_thread_max_priority());
	set_thread_name(recvThread, "voip-recv");
	start_thread(sendThread, StartSendThread, this);
	set_thread_priority(sendThread,
	                    get_thread_max_priority());
	set_thread_name(sendThread, "voip-send");
	start_thread(tickThread, StartTickThread, this);
	set_thread_priority(tickThread,
	                    get_thread_max_priority());
	set_thread_name(tickThread, "voip-tick");
}

size_t VoIPController::AudioInputCallback(
    unsigned char *data, size_t length, void *param)
{
	((VoIPController *)param)->HandleAudioInput(data,
	        length);
	return 0;
}

void VoIPController::HandleAudioInput(
    unsigned char *data, size_t len)
{
	if(stopping) {
		return;
	}
	if(waitingForAcks || dontSendPackets > 0) {
		LOGV("waiting for RLC, dropping outgoing audio packet");
		return;
	}
	int audioPacketGrouping = 1;
	BufferOutputStream *pkt = NULL;
	if(audioPacketsWritten == 0) {
		pkt = GetOutgoingPacketBuffer();
		if(!pkt) {
			LOGW("Dropping data packet, queue overflow");
			return;
		}
		currentAudioPacket = pkt;
	} else {
		pkt = currentAudioPacket;
	}
	unsigned char flags = (unsigned char) (
	                          len > 255 ? STREAM_DATA_FLAG_LEN16 : 0);
	pkt->WriteByte((unsigned char) (1 |
	                                flags)); // streamID + flags
	if(len > 255) {
		pkt->WriteInt16((int16_t)len);
	} else {
		pkt->WriteByte((unsigned char)len);
	}
	pkt->WriteInt32(audioTimestampOut);
	pkt->WriteBytes(data, len);
	audioPacketsWritten++;
	if(audioPacketsWritten >= audioPacketGrouping) {
		uint32_t pl = pkt->GetLength();
		unsigned char tmp[MSC_STACK_FALLBACK(pl, 1024)];
		memcpy(tmp, pkt->GetBuffer(), pl);
		pkt->Reset();
		unsigned char type;
		switch(audioPacketGrouping) {
		case 2:
			type = PKT_STREAM_DATA_X2;
			break;
		case 3:
			type = PKT_STREAM_DATA_X3;
			break;
		default:
			type = PKT_STREAM_DATA;
			break;
		}
		WritePacketHeader(pkt, type, pl);
		pkt->WriteBytes(tmp, pl);
		//LOGI("payload size %u", pl);
		if(pl < 253) {
			pl += 1;
		}
		for(; pl % 4 > 0; pl++) {
			pkt->WriteByte(0);
		}
		sendQueue->Put(pkt);
		audioPacketsWritten = 0;
	}
	audioTimestampOut += outgoingStreams[0]->frameDuration;
}

void VoIPController::Connect()
{
	assert(state != STATE_WAIT_INIT_ACK);
	connectionInitTime = GetCurrentTime();
	SendInit();
}


void VoIPController::SetEncryptionKey(char *key,
                                      bool isOutgoing)
{
	memcpy(encryptionKey, key, 256);
	uint8_t sha1[SHA1_LENGTH];
	crypto.sha1((uint8_t *) encryptionKey, 256, sha1);
	memcpy(keyFingerprint, sha1 + (SHA1_LENGTH - 8), 8);
	uint8_t sha256[SHA256_LENGTH];
	crypto.sha256((uint8_t *) encryptionKey, 256,
	              sha256);
	memcpy(callID, sha256 + (SHA256_LENGTH - 16), 16);
	this->isOutgoing = isOutgoing;
}

uint32_t VoIPController::WritePacketHeader(
    BufferOutputStream *s, unsigned char type,
    uint32_t length)
{
	uint32_t acks = 0;
	int i;
	for(i = 0; i < 32; i++) {
		if(recvPacketTimes[i] > 0) {
			acks |= 1;
		}
		if(i < 31) {
			acks <<= 1;
		}
	}

	uint32_t pseq = seq++;

	if(state == STATE_WAIT_INIT ||
	        state == STATE_WAIT_INIT_ACK) {
		s->WriteInt32(TLID_DECRYPTED_AUDIO_BLOCK);
		int64_t randomID;
		crypto.rand_bytes((uint8_t *) &randomID, 8);
		s->WriteInt64(randomID);
		unsigned char randBytes[7];
		crypto.rand_bytes(randBytes, 7);
		s->WriteByte(7);
		s->WriteBytes(randBytes, 7);
		uint32_t pflags = PFLAG_HAS_RECENT_RECV |
		                  PFLAG_HAS_SEQ;
		if(length > 0) {
			pflags |= PFLAG_HAS_DATA;
		}
		if(state == STATE_WAIT_INIT ||
		        state == STATE_WAIT_INIT_ACK) {
			pflags |= PFLAG_HAS_CALL_ID | PFLAG_HAS_PROTO;
		}
		pflags |= ((uint32_t) type) << 24;
		s->WriteInt32(pflags);

		if(pflags & PFLAG_HAS_CALL_ID) {
			s->WriteBytes(callID, 16);
		}
		s->WriteInt32(lastRemoteSeq);
		s->WriteInt32(pseq);
		s->WriteInt32(acks);
		if(pflags & PFLAG_HAS_PROTO) {
			s->WriteInt32(PROTOCOL_NAME);
		}
		if(length > 0) {
			if(length <= 253) {
				s->WriteByte((unsigned char) length);
			} else {
				s->WriteByte(254);
				s->WriteByte((unsigned char) (length & 0xFF));
				s->WriteByte((unsigned char) ((length >> 8) &
				                              0xFF));
				s->WriteByte((unsigned char) ((length >> 16) &
				                              0xFF));
			}
		}
	} else {
		s->WriteInt32(TLID_SIMPLE_AUDIO_BLOCK);
		int64_t randomID;
		crypto.rand_bytes((uint8_t *) &randomID, 8);
		s->WriteInt64(randomID);
		unsigned char randBytes[7];
		crypto.rand_bytes(randBytes, 7);
		s->WriteByte(7);
		s->WriteBytes(randBytes, 7);
		uint32_t lenWithHeader = length + 13;
		if(lenWithHeader > 0) {
			if(lenWithHeader <= 253) {
				s->WriteByte((unsigned char) lenWithHeader);
			} else {
				s->WriteByte(254);
				s->WriteByte((unsigned char) (lenWithHeader &
				                              0xFF));
				s->WriteByte((unsigned char) ((lenWithHeader >> 8)
				                              & 0xFF));
				s->WriteByte((unsigned char) ((lenWithHeader >>
				                               16) & 0xFF));
			}
		}
		s->WriteByte(type);
		s->WriteInt32(lastRemoteSeq);
		s->WriteInt32(pseq);
		s->WriteInt32(acks);
	}

	if(type == PKT_STREAM_DATA ||
	        type == PKT_STREAM_DATA_X2 ||
	        type == PKT_STREAM_DATA_X3) {
		conctl->PacketSent(pseq, length);
	}

	memmove(&sentPacketTimes[1], sentPacketTimes,
	        31 * sizeof(double));
	sentPacketTimes[0] = GetCurrentTime();
	lastSentSeq = pseq;
	//LOGI("packet header size %d", s->GetLength());

	return pseq;
}


void VoIPController::UpdateAudioBitrate()
{
	if(encoder) {
		if(dataSavingMode || dataSavingRequestedByPeer) {
			maxBitrate = maxAudioBitrateSaving;
			encoder->SetBitrate(initAudioBitrateSaving);
		} else if(networkType == NET_TYPE_GPRS) {
			maxBitrate = maxAudioBitrateGPRS;
			encoder->SetBitrate(initAudioBitrateGPRS);
		} else if(networkType == NET_TYPE_EDGE) {
			maxBitrate = maxAudioBitrateEDGE;
			encoder->SetBitrate(initAudioBitrateEDGE);
		} else {
			maxBitrate = maxAudioBitrate;
			encoder->SetBitrate(initAudioBitrate);
		}
	}
}


void VoIPController::SendInit()
{
	BufferOutputStream *out = new BufferOutputStream(
	    1024);
	WritePacketHeader(out, PKT_INIT, 15);
	out->WriteInt32(PROTOCOL_VERSION);
	out->WriteInt32(MIN_PROTOCOL_VERSION);
	uint32_t flags = 0;
	if(dataSavingMode) {
		flags |= INIT_FLAG_DATA_SAVING_ENABLED;
	}
	out->WriteInt32(flags);
	out->WriteByte(1); // audio codecs count
	out->WriteByte(CODEC_OPUS);
	out->WriteByte(0); // video codecs count
	lock_mutex(endpointsMutex);
	for(std::vector<Endpoint *>::iterator itr =
	            endpoints.begin(); itr != endpoints.end(); ++itr) {
		SendPacket(out->GetBuffer(), out->GetLength(),
		           *itr);
	}
	unlock_mutex(endpointsMutex);
	SetState(STATE_WAIT_INIT_ACK);
	delete out;
}

void VoIPController::SendInitAck()
{

}

void VoIPController::RunRecvThread()
{
	LOGI("Receive thread starting");
	unsigned char buffer[1024];
	NetworkPacket packet;
	while(runReceiver) {
		//LOGI("Before recv");
		packet.data = buffer;
		packet.length = 1024;
		socket->Receive(&packet);
		if(!packet.address) {
			LOGE("Packet has null address. This shouldn't happen.");
			continue;
		}
		size_t len = packet.length;
		if(!len) {
			LOGE("Packet has zero length.");
			continue;
		}
		//LOGV("Received %d bytes from %s:%d at %.5lf", len, inet_ntoa(srcAddr.sin_addr), ntohs(srcAddr.sin_port), GetCurrentTime());
		Endpoint *srcEndpoint = NULL;

		IPv4Address *src4 = dynamic_cast<IPv4Address *>
		                    (packet.address);
		if(src4) {
			lock_mutex(endpointsMutex);
			for(std::vector<Endpoint *>::iterator itrtr =
			            endpoints.begin(); itrtr != endpoints.end();
			        ++itrtr) {
				if((*itrtr)->address == *src4) {
					srcEndpoint = *itrtr;
					break;
				}
			}
			unlock_mutex(endpointsMutex);
		}

		if(!srcEndpoint) {
			LOGW("Received a packet from unknown source %s:%u",
			     packet.address->ToString().c_str(), packet.port);
			continue;
		}
		if(len <= 0) {
			//LOGW("error receiving: %d / %s", errno, strerror(errno));
			continue;
		}
		if(IS_MOBILE_NETWORK(networkType)) {
			stats.bytesRecvdMobile += (uint64_t)len;
		} else {
			stats.bytesRecvdWifi += (uint64_t)len;
		}
		BufferInputStream in(buffer, (size_t)len);
		try {
			if(memcmp(buffer,
			          srcEndpoint->type == EP_TYPE_UDP_RELAY ?
			          srcEndpoint->peerTag : callID, 16) != 0) {
				LOGW("Received packet has wrong peerTag");

				continue;
			}
			in.Seek(16);
			if(waitingForRelayPeerInfo &&
			        in.Remaining() >= 32) {
				bool isPublicIpResponse = true;
				int i;
				for(i = 0; i < 12; i++) {
					if((unsigned char)buffer[in.GetOffset() + i] !=
					        0xFF) {
						isPublicIpResponse = false;
						break;
					}
				}

				if(isPublicIpResponse) {
					waitingForRelayPeerInfo = false;
					in.Seek(in.GetOffset() + 12);
					uint32_t tlid = (uint32_t) in.ReadInt32();
					if(tlid == TLID_UDP_REFLECTOR_PEER_INFO) {
						lock_mutex(endpointsMutex);
						uint32_t myAddr = (uint32_t) in.ReadInt32();
						uint32_t myPort = (uint32_t) in.ReadInt32();
						uint32_t peerAddr = (uint32_t) in.ReadInt32();
						uint32_t peerPort = (uint32_t) in.ReadInt32();
						for(std::vector<Endpoint *>::iterator itrtr =
						            endpoints.begin(); itrtr != endpoints.end();
						        ++itrtr) {
							Endpoint *ep = *itrtr;
							if(ep->type == EP_TYPE_UDP_P2P_INET) {
								if(currentEndpoint == ep) {
									currentEndpoint = preferredRelay;
								}
								delete ep;
								endpoints.erase(itrtr);
								break;
							}
						}
						for(std::vector<Endpoint *>::iterator itrtr =
						            endpoints.begin(); itrtr != endpoints.end();
						        ++itrtr) {
							Endpoint *ep = *itrtr;
							if(ep->type == EP_TYPE_UDP_P2P_LAN) {
								if(currentEndpoint == ep) {
									currentEndpoint = preferredRelay;
								}
								delete ep;
								endpoints.erase(itrtr);
								break;
							}
						}
						IPv4Address _peerAddr(peerAddr);
						IPv6Address emptyV6("::0");
						unsigned char peerTag[16];
						endpoints.push_back(new Endpoint(0,
						                                 (uint16_t) peerPort, _peerAddr, emptyV6,
						                                 EP_TYPE_UDP_P2P_INET, peerTag));
						LOGW("Received reflector peer info, my=%08X:%u, peer=%08X:%u",
						     myAddr, myPort, peerAddr, peerPort);
						if(myAddr == peerAddr) {
							LOGW("Detected LAN");
							IPv4Address lanAddr(0);
							socket->GetLocalInterfaceInfo(&lanAddr, NULL);

							BufferOutputStream pkt(8);
							pkt.WriteInt32(lanAddr.GetAddress());
							pkt.WriteInt32(socket->GetLocalPort());
							SendPacketReliably(PKT_LAN_ENDPOINT,
							                   pkt.GetBuffer(), pkt.GetLength(), 0.5, 10);
						}
						unlock_mutex(endpointsMutex);
					} else {
						LOGE("It looks like a reflector response but tlid is %08X, expected %08X",
						     tlid, TLID_UDP_REFLECTOR_PEER_INFO);
					}

					continue;
				}
			}
			if(in.Remaining() < 40) {

				continue;
			}

			unsigned char fingerprint[8], msgHash[16];
			in.ReadBytes(fingerprint, 8);
			in.ReadBytes(msgHash, 16);
			if(memcmp(fingerprint, keyFingerprint, 8) != 0) {
				LOGW("Received packet has wrong key fingerprint");

				continue;
			}
			unsigned char key[32], iv[32];
			KDF(msgHash, isOutgoing ? 8 : 0, key, iv);
			unsigned char aesOut[MSC_STACK_FALLBACK(
			                         in.Remaining(), 1024)];
			crypto.aes_ige_decrypt((unsigned char *) buffer
			                       + in.GetOffset(), aesOut, in.Remaining(), key, iv);
			memcpy(buffer + in.GetOffset(), aesOut,
			       in.Remaining());
			unsigned char sha[SHA1_LENGTH];
			uint32_t _len = (uint32_t) in.ReadInt32();
			if(_len > in.Remaining()) {
				_len = in.Remaining();
			}
			crypto.sha1((uint8_t *) (buffer + in.GetOffset() - 4),
			            (size_t) (_len + 4), sha);
			if(memcmp(msgHash, sha + (SHA1_LENGTH - 16), 16) != 0) {
				LOGW("Received packet has wrong hash after decryption");

				continue;
			}

			lastRecvPacketTime = GetCurrentTime();


			/*decryptedAudioBlock random_id:long random_bytes:string flags:# voice_call_id:flags.2?int128 in_seq_no:flags.4?int out_seq_no:flags.4?int
			* recent_received_mask:flags.5?int proto:flags.3?int extra:flags.1?string raw_data:flags.0?string = DecryptedAudioBlock
			simpleAudioBlock random_id:long random_bytes:string raw_data:string = DecryptedAudioBlock;
			*/
			uint32_t ackId, pseq, acks;
			unsigned char type;
			uint32_t tlid = (uint32_t) in.ReadInt32();
			uint32_t packetInnerLen;
			if(tlid == TLID_DECRYPTED_AUDIO_BLOCK) {
				in.ReadInt64(); // random id
				uint32_t randLen = (uint32_t) in.ReadTlLength();
				in.Seek(in.GetOffset() + randLen + pad4(randLen));
				uint32_t flags = (uint32_t) in.ReadInt32();
				type = (unsigned char) ((flags >> 24) & 0xFF);
				if(!(flags & PFLAG_HAS_SEQ &&
				        flags & PFLAG_HAS_RECENT_RECV)) {
					LOGW("Received packet doesn't have PFLAG_HAS_SEQ, PFLAG_HAS_RECENT_RECV, or both");

					continue;
				}
				if(flags & PFLAG_HAS_CALL_ID) {
					unsigned char pktCallID[16];
					in.ReadBytes(pktCallID, 16);
					if(memcmp(pktCallID, callID, 16) != 0) {
						LOGW("Received packet has wrong call id");

						lastError = TGVOIP_ERROR_UNKNOWN;
						SetState(STATE_FAILED);
						return;
					}
				}
				ackId = (uint32_t) in.ReadInt32();
				pseq = (uint32_t) in.ReadInt32();
				acks = (uint32_t) in.ReadInt32();
				if(flags & PFLAG_HAS_PROTO) {
					uint32_t proto = (uint32_t) in.ReadInt32();
					if(proto != PROTOCOL_NAME) {
						LOGW("Received packet uses wrong protocol");

						lastError = TGVOIP_ERROR_INCOMPATIBLE;
						SetState(STATE_FAILED);
						return;
					}
				}
				if(flags & PFLAG_HAS_EXTRA) {
					uint32_t extraLen = (uint32_t) in.ReadTlLength();
					in.Seek(in.GetOffset() + extraLen + pad4(extraLen));
				}
				if(flags & PFLAG_HAS_DATA) {
					packetInnerLen = in.ReadTlLength();
				}
			} else if(tlid == TLID_SIMPLE_AUDIO_BLOCK) {
				in.ReadInt64(); // random id
				uint32_t randLen = (uint32_t) in.ReadTlLength();
				in.Seek(in.GetOffset() + randLen + pad4(randLen));
				packetInnerLen = in.ReadTlLength();
				type = in.ReadByte();
				ackId = (uint32_t) in.ReadInt32();
				pseq = (uint32_t) in.ReadInt32();
				acks = (uint32_t) in.ReadInt32();
			} else {
				LOGW("Received a packet of unknown type %08X",
				     tlid);

				continue;
			}
			packetsRecieved++;
			if(seqgt(pseq, lastRemoteSeq)) {
				uint32_t diff = pseq - lastRemoteSeq;
				if(diff > 31) {
					memset(recvPacketTimes, 0, 32 * sizeof(double));
				} else {
					memmove(&recvPacketTimes[diff], recvPacketTimes,
					        (32 - diff)*sizeof(double));
					if(diff > 1) {
						memset(recvPacketTimes, 0, diff * sizeof(double));
					}
					recvPacketTimes[0] = GetCurrentTime();
				}
				lastRemoteSeq = pseq;
			} else if(!seqgt(pseq, lastRemoteSeq) &&
			          lastRemoteSeq - pseq < 32) {
				if(recvPacketTimes[lastRemoteSeq - pseq] != 0) {
					LOGW("Received duplicated packet for seq %u",
					     pseq);

					continue;
				}
				recvPacketTimes[lastRemoteSeq - pseq] =
				    GetCurrentTime();
			} else if(lastRemoteSeq - pseq >= 32) {
				LOGW("Packet %u is out of order and too late",
				     pseq);

				continue;
			}
			if(seqgt(ackId, lastRemoteAckSeq)) {
				uint32_t diff = ackId - lastRemoteAckSeq;
				if(diff > 31) {
					memset(remoteAcks, 0, 32 * sizeof(double));
				} else {
					memmove(&remoteAcks[diff], remoteAcks,
					        (32 - diff)*sizeof(double));
					if(diff > 1) {
						memset(remoteAcks, 0, diff * sizeof(double));
					}
					remoteAcks[0] = GetCurrentTime();
				}
				if(waitingForAcks &&
				        lastRemoteAckSeq >= firstSentPing) {
					memset(rttHistory, 0, 32 * sizeof(double));
					waitingForAcks = false;
					dontSendPackets = 10;
					LOGI("resuming sending");
				}
				lastRemoteAckSeq = ackId;
				conctl->PacketAcknowledged(ackId);
				int i;
				for(i = 0; i < 31; i++) {
					if(remoteAcks[i + 1] == 0) {
						if((acks >> (31 - i)) & 1) {
							remoteAcks[i + 1] = GetCurrentTime();
							conctl->PacketAcknowledged(ackId - (i + 1));
						}
					}
				}
				lock_mutex(queuedPacketsMutex);
				for(i = 0; i < queuedPackets.size(); i++) {
					voip_queued_packet_t *qp = queuedPackets[i];
					int j;
					bool didAck = false;
					for(j = 0; j < 16; j++) {
						LOGD("queued packet %u, seq %u=%u", i, j,
						     qp->seqs[j]);
						if(qp->seqs[j] == 0) {
							break;
						}
						int remoteAcksIndex = lastRemoteAckSeq - qp->seqs[j];
						LOGV("remote acks index %u, value %f",
						     remoteAcksIndex, remoteAcksIndex >= 0 &&
						     remoteAcksIndex < 32 ? remoteAcks[remoteAcksIndex] :
						     -1);
						if(seqgt(lastRemoteAckSeq, qp->seqs[j]) &&
						        remoteAcksIndex >= 0 && remoteAcksIndex < 32 &&
						        remoteAcks[remoteAcksIndex] > 0) {
							LOGD("did ack seq %u, removing", qp->seqs[j]);
							didAck = true;
							break;
						}
					}
					if(didAck) {
						if(qp->data) {
							free(qp->data);
						}
						free(qp);
						queuedPackets.erase(queuedPackets.begin() + i);
						i--;
						continue;
					}
				}
				unlock_mutex(queuedPacketsMutex);
			}

			if(srcEndpoint != currentEndpoint &&
			        srcEndpoint->type == EP_TYPE_UDP_RELAY &&
			        currentEndpoint->type != EP_TYPE_UDP_RELAY) {
				if(seqgt(lastSentSeq - 32, lastRemoteAckSeq)) {
					currentEndpoint = srcEndpoint;
					LOGI("Peer network address probably changed, switching to relay");
					if(allowP2p) {
						SendPublicEndpointsRequest();
					}
				}
			}
			//LOGV("acks: %u -> %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf", lastRemoteAckSeq, remoteAcks[0], remoteAcks[1], remoteAcks[2], remoteAcks[3], remoteAcks[4], remoteAcks[5], remoteAcks[6], remoteAcks[7]);
			//LOGD("recv: %u -> %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf", lastRemoteSeq, recvPacketTimes[0], recvPacketTimes[1], recvPacketTimes[2], recvPacketTimes[3], recvPacketTimes[4], recvPacketTimes[5], recvPacketTimes[6], recvPacketTimes[7]);
			//LOGI("RTT = %.3lf", GetAverageRTT());
			//LOGV("Packet %u type is %d", pseq, type);
			if(type == PKT_INIT) {
				LOGD("Received init");
				if(!receivedInit) {
					receivedInit = true;
					currentEndpoint = srcEndpoint;
					if(srcEndpoint->type == EP_TYPE_UDP_RELAY) {
						preferredRelay = srcEndpoint;
					}
					LogDebugInfo();
				}
				peerVersion = (uint32_t) in.ReadInt32();
				LOGI("Peer version is %d", peerVersion);
				uint32_t minVer = (uint32_t) in.ReadInt32();
				if(minVer > PROTOCOL_VERSION ||
				        peerVersion < MIN_PROTOCOL_VERSION) {
					lastError = TGVOIP_ERROR_INCOMPATIBLE;

					SetState(STATE_FAILED);
					return;
				}
				uint32_t flags = (uint32_t) in.ReadInt32();
				if(flags & INIT_FLAG_DATA_SAVING_ENABLED) {
					dataSavingRequestedByPeer = true;
					UpdateDataSavingState();
					UpdateAudioBitrate();
				}

				int i;
				int numSupportedAudioCodecs = in.ReadByte();
				for(i = 0; i < numSupportedAudioCodecs; i++) {
					in.ReadByte(); // ignore for now
				}
				int numSupportedVideoCodecs = in.ReadByte();
				for(i = 0; i < numSupportedVideoCodecs; i++) {
					in.ReadByte(); // ignore for now
				}

				BufferOutputStream *out = new BufferOutputStream(
				    1024);
				WritePacketHeader(out, PKT_INIT_ACK,
				                  (peerVersion >= 2 ? 10 : 2) + (peerVersion >= 2 ? 6 : 4)
				                  *outgoingStreams.size());
				if(peerVersion >= 2) {
					out->WriteInt32(PROTOCOL_VERSION);
					out->WriteInt32(MIN_PROTOCOL_VERSION);
				}

				out->WriteByte((unsigned char)
				               outgoingStreams.size());
				for(i = 0; i < outgoingStreams.size(); i++) {
					out->WriteByte(outgoingStreams[i]->id);
					out->WriteByte(outgoingStreams[i]->type);
					out->WriteByte(outgoingStreams[i]->codec);
					if(peerVersion >= 2) {
						out->WriteInt16(
						    outgoingStreams[i]->frameDuration);
					} else {
						outgoingStreams[i]->frameDuration = 20;
					}
					out->WriteByte((unsigned char) (
					                   outgoingStreams[i]->enabled ? 1 : 0));
				}
				SendPacket(out->GetBuffer(), out->GetLength(),
				           currentEndpoint);
				delete out;
			}
			if(type == PKT_INIT_ACK) {
				LOGD("Received init ack");

				if(!receivedInitAck) {
					receivedInitAck = true;
					if(packetInnerLen > 10) {
						peerVersion = in.ReadInt32();
						uint32_t minVer = (uint32_t) in.ReadInt32();
						if(minVer > PROTOCOL_VERSION ||
						        peerVersion < MIN_PROTOCOL_VERSION) {
							lastError = TGVOIP_ERROR_INCOMPATIBLE;

							SetState(STATE_FAILED);
							return;
						}
					} else {
						peerVersion = 1;
					}

					LOGI("peer version from init ack %d",
					     peerVersion);

					unsigned char streamCount = in.ReadByte();
					if(streamCount == 0) {
						continue;
					}

					int i;
					voip_stream_t *incomingAudioStream = NULL;
					for(i = 0; i < streamCount; i++) {
						voip_stream_t *stm = (voip_stream_t *) malloc(
						                         sizeof(voip_stream_t));
						stm->id = in.ReadByte();
						stm->type = in.ReadByte();
						stm->codec = in.ReadByte();
						if(peerVersion >= 2) {
							stm->frameDuration = (uint16_t) in.ReadInt16();
						} else {
							stm->frameDuration = 20;
						}
						stm->enabled = in.ReadByte() == 1;
						incomingStreams.push_back(stm);
						if(stm->type == STREAM_TYPE_AUDIO &&
						        !incomingAudioStream) {
							incomingAudioStream = stm;
						}
					}
					if(!incomingAudioStream) {
						continue;
					}

					voip_stream_t *outgoingAudioStream =
					    outgoingStreams[0];

					if(!audioInput) {
						LOGI("before create audio io");
						audioInput = tgvoip::audio::AudioInput::Create(
						                 currentAudioInput);
						audioInput->Configure(48000, 16, 1);
						audioOutput = tgvoip::audio::AudioOutput::Create(
						                  currentAudioOutput);
						audioOutput->Configure(48000, 16, 1);
						echoCanceller = new EchoCanceller(config.enableAEC,
						                                  config.enableNS, config.enableAGC);
						encoder = new OpusEncoder(audioInput);
						encoder->SetCallback(AudioInputCallback, this);
						encoder->SetOutputFrameDuration(
						    outgoingAudioStream->frameDuration);
						encoder->SetEchoCanceller(echoCanceller);
						encoder->Start();
						if(!micMuted) {
							audioInput->Start();
							if(!audioInput->IsInitialized()) {
								LOGE("Erorr initializing audio capture");
								lastError = TGVOIP_ERROR_AUDIO_IO;

								SetState(STATE_FAILED);
								return;
							}
						}
						if(!audioOutput->IsInitialized()) {
							LOGE("Erorr initializing audio playback");
							lastError = TGVOIP_ERROR_AUDIO_IO;

							SetState(STATE_FAILED);
							return;
						}
						UpdateAudioBitrate();

						jitterBuffer = new JitterBuffer(NULL,
						                                incomingAudioStream->frameDuration);
						decoder = new OpusDecoder(audioOutput);
						decoder->SetEchoCanceller(echoCanceller);
						decoder->SetJitterBuffer(jitterBuffer);
						decoder->SetFrameDuration(
						    incomingAudioStream->frameDuration);
						decoder->Start();
						if(incomingAudioStream->frameDuration > 50) {
							jitterBuffer->SetMinPacketCount(
							    ServerConfig::GetSharedInstance()->GetInt("jitter_initial_delay_60",
							            3));
						} else if(incomingAudioStream->frameDuration > 30) {
							jitterBuffer->SetMinPacketCount(
							    ServerConfig::GetSharedInstance()->GetInt("jitter_initial_delay_40",
							            4));
						} else {
							jitterBuffer->SetMinPacketCount(
							    ServerConfig::GetSharedInstance()->GetInt("jitter_initial_delay_20",
							            6));
						}
						//audioOutput->Start();
#ifdef TGVOIP_USE_AUDIO_SESSION
#ifdef __APPLE__
						if(acquireAudioSession) {
							acquireAudioSession(^() {
								LOGD("Audio session acquired");
								needNotifyAcquiredAudioSession = true;
							});
						} else {
							audio::AudioUnitIO::AudioSessionAcquired();
						}
#endif
#endif
					}
					SetState(STATE_ESTABLISHED);
					if(allowP2p) {
						SendPublicEndpointsRequest();
					}
				}
			}
			if(type == PKT_STREAM_DATA ||
			        type == PKT_STREAM_DATA_X2 ||
			        type == PKT_STREAM_DATA_X3) {
				int count;
				switch(type) {
				case PKT_STREAM_DATA_X2:
					count = 2;
					break;
				case PKT_STREAM_DATA_X3:
					count = 3;
					break;
				case PKT_STREAM_DATA:
				default:
					count = 1;
					break;
				}
				int i;
				if(srcEndpoint->type == EP_TYPE_UDP_RELAY &&
				        srcEndpoint != peerPreferredRelay) {
					peerPreferredRelay = srcEndpoint;
				}
				for(i = 0; i < count; i++) {
					unsigned char streamID = in.ReadByte();
					unsigned char flags = (unsigned char) (
					                          streamID & 0xC0);
					uint16_t sdlen = (uint16_t) (flags &
					                             STREAM_DATA_FLAG_LEN16 ? in.ReadInt16() :
					                             in.ReadByte());
					uint32_t pts = (uint32_t) in.ReadInt32();
					//LOGD("stream data, pts=%d, len=%d, rem=%d", pts, sdlen, in.Remaining());
					audioTimestampIn = pts;
					if(!audioOutStarted && audioOutput) {
						audioOutput->Start();
						audioOutStarted = true;
					}
					if(jitterBuffer) {
						jitterBuffer->HandleInput((unsigned char *) (
						                              buffer + in.GetOffset()), sdlen, pts);
					}
					if(i < count - 1) {
						in.Seek(in.GetOffset() + sdlen);
					}
				}
			}
			if(type == PKT_PING) {
				LOGD("Received ping from %s:%d",
				     packet.address->ToString().c_str(),
				     srcEndpoint->port);
				if(srcEndpoint->type != EP_TYPE_UDP_RELAY &&
				        !allowP2p) {
					LOGW("Received p2p ping but p2p is disabled by manual override");

					continue;
				}
				if(srcEndpoint == currentEndpoint) {
					BufferOutputStream *pkt = GetOutgoingPacketBuffer();
					if(!pkt) {
						LOGW("Dropping pong packet, queue overflow");

						continue;
					}
					WritePacketHeader(pkt, PKT_PONG, 4);
					pkt->WriteInt32(pseq);
					sendQueue->Put(pkt);
				} else {
					BufferOutputStream pkt(32);
					WritePacketHeader(&pkt, PKT_PONG, 4);
					pkt.WriteInt32(pseq);
					SendPacket(pkt.GetBuffer(), pkt.GetLength(),
					           srcEndpoint);
				}
			}
			if(type == PKT_PONG) {
				if(packetInnerLen >= 4) {
					uint32_t pingSeq = (uint32_t) in.ReadInt32();
					if(pingSeq == srcEndpoint->lastPingSeq) {
						memmove(&srcEndpoint->rtts[1], srcEndpoint->rtts,
						        sizeof(double) * 5);
						srcEndpoint->rtts[0] = GetCurrentTime()
						                       - srcEndpoint->lastPingTime;
						int i;
						srcEndpoint->averageRTT = 0;
						for(i = 0; i < 6; i++) {
							if(srcEndpoint->rtts[i] == 0) {
								break;
							}
							srcEndpoint->averageRTT += srcEndpoint->rtts[i];
						}
						srcEndpoint->averageRTT /= i;
						LOGD("Current RTT via %s: %.3f, average: %.3f",
						     packet.address->ToString().c_str(),
						     srcEndpoint->rtts[0], srcEndpoint->averageRTT);
					}
				}
				/*if(currentEndpoint!=srcEndpoint && (srcEndpoint->type==EP_TYPE_UDP_P2P_INET || srcEndpoint->type==EP_TYPE_UDP_P2P_LAN)){
					LOGI("Switching to P2P now!");
					currentEndpoint=srcEndpoint;
					needSendP2pPing=false;
				}*/
			}
			if(type == PKT_STREAM_STATE) {
				unsigned char id = in.ReadByte();
				unsigned char enabled = in.ReadByte();
				int i;
				for(i = 0; i < incomingStreams.size(); i++) {
					if(incomingStreams[i]->id == id) {
						incomingStreams[i]->enabled = enabled == 1;
						UpdateAudioOutputState();
						break;
					}
				}
			}
			if(type == PKT_LAN_ENDPOINT) {
				LOGV("received lan endpoint");
				uint32_t peerAddr = (uint32_t) in.ReadInt32();
				uint16_t peerPort = (uint16_t) in.ReadInt32();
				lock_mutex(endpointsMutex);
				for(std::vector<Endpoint *>::iterator itrtr =
				            endpoints.begin(); itrtr != endpoints.end();
				        ++itrtr) {
					if((*itrtr)->type == EP_TYPE_UDP_P2P_LAN) {
						if(currentEndpoint == *itrtr) {
							currentEndpoint = preferredRelay;
						}
						delete *itrtr;
						endpoints.erase(itrtr);
						break;
					}
				}
				IPv4Address v4addr(peerAddr);
				IPv6Address v6addr("::0");
				unsigned char peerTag[16];
				endpoints.push_back(new Endpoint(0, peerPort,
				                                 v4addr, v6addr, EP_TYPE_UDP_P2P_LAN, peerTag));
				unlock_mutex(endpointsMutex);
			}
			if(type == PKT_NETWORK_CHANGED) {
				currentEndpoint = preferredRelay;
				if(allowP2p) {
					SendPublicEndpointsRequest();
				}
				if(peerVersion >= 2) {
					uint32_t flags = (uint32_t) in.ReadInt32();
					dataSavingRequestedByPeer = (flags &
					                             INIT_FLAG_DATA_SAVING_ENABLED) ==
					                            INIT_FLAG_DATA_SAVING_ENABLED;
					UpdateDataSavingState();
					UpdateAudioBitrate();
				}
			}
			/*if(type==PKT_SWITCH_PREF_RELAY){
				uint64_t relayId=(uint64_t) in.ReadInt64();
				int i;
				for(i=0;i<endpoints.size();i++){
					if(endpoints[i]->type==EP_TYPE_UDP_RELAY && endpoints[i]->id==relayId){
						preferredRelay=endpoints[i];
						LOGD("Switching preferred relay to %s:%d", inet_ntoa(preferredRelay->address), preferredRelay->port);
						break;
					}
				}
				if(currentEndpoint->type==EP_TYPE_UDP_RELAY)
					currentEndpoint=preferredRelay;
			}*/
			/*if(type==PKT_SWITCH_TO_P2P && allowP2p){
				voip_endpoint_t* p2p=GetEndpointByType(EP_TYPE_UDP_P2P_INET);
				if(p2p){
					voip_endpoint_t* lan=GetEndpointByType(EP_TYPE_UDP_P2P_LAN);
					if(lan && lan->_averageRtt>0){
						LOGI("Switching to p2p (LAN)");
						currentEndpoint=lan;
					}else{
						if(lan)
							lan->_lastPingTime=0;
						if(p2p->_averageRtt>0){
							LOGI("Switching to p2p (Inet)");
							currentEndpoint=p2p;
						}else{
							p2p->_lastPingTime=0;
						}
					}
				}
			}*/
		} catch(std::out_of_range x) {
			LOGW("Error parsing packet: %s", x.what());
		}
	}
	LOGI("=== recv thread exiting ===");
}

void VoIPController::RunSendThread()
{
	while(runReceiver) {
		BufferOutputStream *pkt = (BufferOutputStream *)
		                          sendQueue->GetBlocking();
		if(pkt) {
			lock_mutex(endpointsMutex);
			SendPacket(pkt->GetBuffer(), pkt->GetLength(),
			           currentEndpoint);
			unlock_mutex(endpointsMutex);
			pkt->Reset();
			lock_mutex(sendBufferMutex);
			emptySendBuffers.push_back(pkt);
			unlock_mutex(sendBufferMutex);
		}
	}
	LOGI("=== send thread exiting ===");
}


void VoIPController::RunTickThread()
{
	uint32_t tickCount = 0;
	bool wasWaitingForAcks = false;
	double startTime = GetCurrentTime();
	while(runReceiver) {
#ifndef _WIN32
		usleep(100000);
#else
		Sleep(100);
#endif
		tickCount++;
		if(tickCount % 5 == 0 && state == STATE_ESTABLISHED) {
			memmove(&rttHistory[1], rttHistory,
			        31 * sizeof(double));
			rttHistory[0] = GetAverageRTT();
			/*if(rttHistory[16]>0){
				LOGI("rtt diff: %.3lf", rttHistory[0]-rttHistory[16]);
			}*/
			int i;
			double v = 0;
			for(i = 1; i < 32; i++) {
				v += rttHistory[i - 1] - rttHistory[i];
			}
			v = v / 32;
			if(rttHistory[0] > 10.0 && rttHistory[8] > 10.0 &&
			        (networkType == NET_TYPE_EDGE ||
			         networkType == NET_TYPE_GPRS)) {
				waitingForAcks = true;
			} else {
				waitingForAcks = false;
			}
			if(waitingForAcks) {
				wasWaitingForAcks = false;
			}
			//LOGI("%.3lf/%.3lf, rtt diff %.3lf, waiting=%d, queue=%d", rttHistory[0], rttHistory[8], v, waitingForAcks, sendQueue->Size());
			if(jitterBuffer) {
				int lostCount =
				    jitterBuffer->GetAndResetLostPacketCount();
				if(lostCount > 0 || (lostCount < 0 &&
				                     recvLossCount > ((uint32_t) - lostCount))) {
					recvLossCount += lostCount;
				}
			}
		}
		if(dontSendPackets > 0) {
			dontSendPackets--;
		}

		int i;

		conctl->Tick();

		if(state == STATE_ESTABLISHED) {
			if((audioInput && !audioInput->IsInitialized()) ||
			        (audioOutput && !audioOutput->IsInitialized())) {
				LOGE("Audio I/O failed");
				lastError = TGVOIP_ERROR_AUDIO_IO;
				SetState(STATE_FAILED);
			}

			int act = conctl->GetBandwidthControlAction();
			if(act == TGVOIP_CONCTL_ACT_DECREASE) {
				uint32_t bitrate = encoder->GetBitrate();
				if(bitrate > 8000) {
					encoder->SetBitrate(bitrate < (minAudioBitrate
					                               + audioBitrateStepDecr) ? minAudioBitrate :
					                    (bitrate - audioBitrateStepDecr));
				}
			} else if(act == TGVOIP_CONCTL_ACT_INCREASE) {
				uint32_t bitrate = encoder->GetBitrate();
				if(bitrate < maxBitrate) {
					encoder->SetBitrate(bitrate + audioBitrateStepIncr);
				}
			}

			if(tickCount % 10 == 0 && encoder) {
				uint32_t sendLossCount = conctl->GetSendLossCount();
				memmove(sendLossCountHistory + 1,
				        sendLossCountHistory, 31 * sizeof(uint32_t));
				sendLossCountHistory[0] = sendLossCount
				                          - prevSendLossCount;
				prevSendLossCount = sendLossCount;
				double avgSendLossCount = 0;
				for(i = 0; i < 10; i++) {
					avgSendLossCount += sendLossCountHistory[i];
				}
				double packetsPerSec = 1000 / (double)
				                       outgoingStreams[0]->frameDuration;
				avgSendLossCount =
				    avgSendLossCount / 10 / packetsPerSec;
				//LOGV("avg send loss: %.1f%%", avgSendLossCount*100);

				if(avgSendLossCount > 0.1) {
					encoder->SetPacketLoss(40);
				} else if(avgSendLossCount > 0.075) {
					encoder->SetPacketLoss(35);
				} else if(avgSendLossCount > 0.0625) {
					encoder->SetPacketLoss(30);
				} else if(avgSendLossCount > 0.05) {
					encoder->SetPacketLoss(25);
				} else if(avgSendLossCount > 0.025) {
					encoder->SetPacketLoss(20);
				} else if(avgSendLossCount > 0.01) {
					encoder->SetPacketLoss(17);
				} else {
					encoder->SetPacketLoss(15);
				}
			}
		}

		bool areThereAnyEnabledStreams = false;

		for(i = 0; i < outgoingStreams.size(); i++) {
			if(outgoingStreams[i]->enabled) {
				areThereAnyEnabledStreams = true;
			}
		}

		if((waitingForAcks && tickCount % 10 == 0) ||
		        (!areThereAnyEnabledStreams && tickCount % 2 == 0)) {
			BufferOutputStream *pkt = GetOutgoingPacketBuffer();
			if(!pkt) {
				LOGW("Dropping ping packet, queue overflow");
				return;
			}
			uint32_t seq = WritePacketHeader(pkt, PKT_NOP, 0);
			firstSentPing = seq;
			sendQueue->Put(pkt);
			LOGV("sent ping");
		}

		if(state == STATE_WAIT_INIT_ACK &&
		        GetCurrentTime() - stateChangeTime > .5) {
			SendInit();
		}

		/*if(needSendP2pPing){
			if(GetCurrentTime()-lastP2pPingTime>2){
				if(p2pPingCount<10){ // try hairpin routing first, even if we have a LAN address
					SendP2pPing(EP_TYPE_UDP_P2P_INET);
				}
				if(p2pPingCount>=5 && p2pPingCount<15){ // last resort to get p2p
					SendP2pPing(EP_TYPE_UDP_P2P_LAN);
				}
				p2pPingCount++;
			}
		}*/

		if(waitingForRelayPeerInfo &&
		        GetCurrentTime() - publicEndpointsReqTime > 5) {
			LOGD("Resending peer relay info request");
			SendPublicEndpointsRequest();
		}

		lock_mutex(queuedPacketsMutex);
		for(i = 0; i < queuedPackets.size(); i++) {
			voip_queued_packet_t *qp = queuedPackets[i];
			if(qp->timeout > 0 && qp->firstSentTime > 0 &&
			        GetCurrentTime() - qp->firstSentTime >= qp->timeout) {
				LOGD("Removing queued packet because of timeout");
				if(qp->data) {
					free(qp->data);
				}
				free(qp);
				queuedPackets.erase(queuedPackets.begin() + i);
				i--;
				continue;
			}
			if(GetCurrentTime() - qp->lastSentTime >=
			        qp->retryInterval) {
				BufferOutputStream *pkt = GetOutgoingPacketBuffer();
				if(pkt) {
					uint32_t seq = WritePacketHeader(pkt, qp->type,
					                                 qp->length);
					memmove(&qp->seqs[1], qp->seqs, 4 * 9);
					qp->seqs[0] = seq;
					qp->lastSentTime = GetCurrentTime();
					LOGD("Sending queued packet, seq=%u, type=%u, len=%u",
					     seq, qp->type, unsigned(qp->length));
					if(qp->firstSentTime == 0) {
						qp->firstSentTime = qp->lastSentTime;
					}
					if(qp->length) {
						pkt->WriteBytes(qp->data, qp->length);
					}
					sendQueue->Put(pkt);
				}
			}
		}
		unlock_mutex(queuedPacketsMutex);

		if(jitterBuffer) {
			jitterBuffer->Tick();
		}

		if(state == STATE_ESTABLISHED) {
			lock_mutex(endpointsMutex);
			Endpoint *minPingRelay = preferredRelay;
			double minPing = preferredRelay->averageRTT;
			for(std::vector<Endpoint *>::iterator e =
			            endpoints.begin(); e != endpoints.end(); ++e) {
				Endpoint *endpoint = *e;
				if(GetCurrentTime() - endpoint->lastPingTime >= 10) {
					LOGV("Sending ping to %s",
					     endpoint->address.ToString().c_str());
					BufferOutputStream pkt(32);
					uint32_t seq = WritePacketHeader(&pkt, PKT_PING, 0);
					endpoint->lastPingTime = GetCurrentTime();
					endpoint->lastPingSeq = seq;
					SendPacket(pkt.GetBuffer(), pkt.GetLength(),
					           endpoint);
				}
				if(endpoint->type == EP_TYPE_UDP_RELAY) {
					if(endpoint->averageRTT > 0 &&
					        endpoint->averageRTT < minPing * relaySwitchThreshold) {
						minPing = endpoint->averageRTT;
						minPingRelay = endpoint;
					}
				}
			}
			if(minPingRelay != preferredRelay) {
				preferredRelay = minPingRelay;
				LOGV("set preferred relay to %s",
				     preferredRelay->address.ToString().c_str());
				if(currentEndpoint->type == EP_TYPE_UDP_RELAY) {
					currentEndpoint = preferredRelay;
				}
				LogDebugInfo();
				/*BufferOutputStream pkt(32);
				pkt.WriteInt64(preferredRelay->id);
				SendPacketReliably(PKT_SWITCH_PREF_RELAY, pkt.GetBuffer(), pkt.GetLength(), 1, 9);*/
			}
			if(currentEndpoint->type == EP_TYPE_UDP_RELAY) {
				Endpoint *p2p = GetEndpointByType(
				                    EP_TYPE_UDP_P2P_INET);
				if(p2p) {
					Endpoint *lan = GetEndpointByType(
					                    EP_TYPE_UDP_P2P_LAN);
					if(lan && lan->averageRTT > 0 &&
					        lan->averageRTT < minPing * relayToP2pSwitchThreshold) {
						//SendPacketReliably(PKT_SWITCH_TO_P2P, NULL, 0, 1, 5);
						currentEndpoint = lan;
						LOGI("Switching to p2p (LAN)");
						LogDebugInfo();
					} else {
						if(p2p->averageRTT > 0 &&
						        p2p->averageRTT < minPing * relayToP2pSwitchThreshold) {
							//SendPacketReliably(PKT_SWITCH_TO_P2P, NULL, 0, 1, 5);
							currentEndpoint = p2p;
							LOGI("Switching to p2p (Inet)");
							LogDebugInfo();
						}
					}
				}
			} else {
				if(minPing > 0 &&
				        minPing < currentEndpoint->averageRTT * p2pToRelaySwitchThreshold) {
					LOGI("Switching to relay");
					currentEndpoint = preferredRelay;
					LogDebugInfo();
				}
			}
			unlock_mutex(endpointsMutex);
		}

		if(state == STATE_ESTABLISHED) {
			if(GetCurrentTime() - lastRecvPacketTime >=
			        config.recv_timeout) {
				if(currentEndpoint &&
				        currentEndpoint->type != EP_TYPE_UDP_RELAY) {
					LOGW("Packet receive timeout, switching to relay");
					currentEndpoint = preferredRelay;
					for(std::vector<Endpoint *>::iterator itrtr =
					            endpoints.begin(); itrtr != endpoints.end();
					        ++itrtr) {
						Endpoint *e = *itrtr;
						if(e->type == EP_TYPE_UDP_P2P_INET ||
						        e->type == EP_TYPE_UDP_P2P_LAN) {
							e->averageRTT = 0;
							memset(e->rtts, 0, sizeof(e->rtts));
						}
					}
					if(allowP2p) {
						SendPublicEndpointsRequest();
					}
					UpdateDataSavingState();
					UpdateAudioBitrate();
					BufferOutputStream s(4);
					s.WriteInt32(dataSavingMode ?
					             INIT_FLAG_DATA_SAVING_ENABLED : 0);
					SendPacketReliably(PKT_NETWORK_CHANGED,
					                   s.GetBuffer(), s.GetLength(), 1, 20);
					lastRecvPacketTime = GetCurrentTime();
				} else {
					LOGW("Packet receive timeout, disconnecting");
					lastError = TGVOIP_ERROR_TIMEOUT;
					SetState(STATE_FAILED);
				}
			}
		} else if(state == STATE_WAIT_INIT ||
		          state == STATE_WAIT_INIT_ACK) {
			if(GetCurrentTime() - connectionInitTime >=
			        config.init_timeout) {
				LOGW("Init timeout, disconnecting");
				lastError = TGVOIP_ERROR_TIMEOUT;
				SetState(STATE_FAILED);
			}
		}

		if(statsDump) {
			//fprintf(statsDump, "Time\tRTT\tLISeq\tLASeq\tCWnd\tBitrate\tJitter\tJDelay\tAJDelay\n");
			fprintf(statsDump,
			        "%.3f\t%.3f\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%.3f\t%.3f\t%.3f\n",
			        GetCurrentTime() - startTime,
			        currentEndpoint->rtts[0],
			        lastRemoteSeq,
			        seq,
			        lastRemoteAckSeq,
			        recvLossCount,
			        conctl ? conctl->GetSendLossCount() : 0,
			        conctl ? (int)conctl->GetInflightDataSize() : 0,
			        encoder ? encoder->GetBitrate() : 0,
			        encoder ? encoder->GetPacketLoss() : 0,
			        jitterBuffer ?
			        jitterBuffer->GetLastMeasuredJitter() : 0,
			        jitterBuffer ?
			        jitterBuffer->GetLastMeasuredDelay() * 0.06 : 0,
			        jitterBuffer ? jitterBuffer->GetCurrentDelay()
			        * 0.06 : 0);
		}

#if defined(__APPLE__) && defined(TGVOIP_USE_AUDIO_SESSION)
		if(needNotifyAcquiredAudioSession) {
			needNotifyAcquiredAudioSession = false;
			audio::AudioUnitIO::AudioSessionAcquired();
		}
#endif
	}
	LOGI("=== tick thread exiting ===");
}


Endpoint &VoIPController::GetRemoteEndpoint()
{
	//return useLan ? &remoteLanEp : &remotePublicEp;
	return *currentEndpoint;
}


void VoIPController::SendPacket(unsigned char
                                *data, size_t len, Endpoint *ep)
{
	if(stopping) {
		return;
	}
	//dst.sin_addr=ep->address;
	//dst.sin_port=htons(ep->port);
	//dst.sin_family=AF_INET;
	BufferOutputStream out(len + 128);
	if(ep->type == EP_TYPE_UDP_RELAY) {
		out.WriteBytes((unsigned char *)ep->peerTag, 16);
	} else {
		out.WriteBytes(callID, 16);
	}
	if(len > 0) {
		BufferOutputStream inner(len + 128);
		inner.WriteInt32(len);
		inner.WriteBytes(data, len);
		if(inner.GetLength() % 16 != 0) {
			size_t padLen = 16 - inner.GetLength() % 16;
			unsigned char padding[16];
			crypto.rand_bytes((uint8_t *) padding, padLen);
			inner.WriteBytes(padding, padLen);
		}
		assert(inner.GetLength() % 16 == 0);
		unsigned char key[32], iv[32],
		         msgHash[SHA1_LENGTH];
		crypto.sha1((uint8_t *) inner.GetBuffer(), len + 4,
		            msgHash);
		out.WriteBytes(keyFingerprint, 8);
		out.WriteBytes((msgHash + (SHA1_LENGTH - 16)), 16);
		KDF(msgHash + (SHA1_LENGTH - 16), isOutgoing ? 0 : 8,
		    key, iv);
		unsigned char aesOut[MSC_STACK_FALLBACK(
		                         inner.GetLength(), 1024)];
		crypto.aes_ige_encrypt(inner.GetBuffer(), aesOut,
		                       inner.GetLength(), key, iv);
		out.WriteBytes(aesOut, inner.GetLength());
	}
	//LOGV("Sending %d bytes to %s:%d", out.GetLength(), ep->address.ToString().c_str(), ep->port);
	if(IS_MOBILE_NETWORK(networkType)) {
		stats.bytesSentMobile += (uint64_t)out.GetLength();
	} else {
		stats.bytesSentWifi += (uint64_t)out.GetLength();
	}

	NetworkPacket pkt;
	pkt.address = (NetworkAddress *)&ep->address;
	pkt.port = ep->port;
	pkt.length = out.GetLength();
	pkt.data = out.GetBuffer();
	socket->Send(&pkt);
}


void VoIPController::SetNetworkType(int type)
{
	networkType = type;
	UpdateDataSavingState();
	UpdateAudioBitrate();
	std::string itfName = socket->GetLocalInterfaceInfo(
	                          NULL, NULL);
	if(itfName != activeNetItfName) {
		socket->OnActiveInterfaceChanged();
		LOGI("Active network interface changed: %s -> %s",
		     activeNetItfName.c_str(), itfName.c_str());
		bool isFirstChange = activeNetItfName.length() == 0;
		activeNetItfName = itfName;
		if(isFirstChange) {
			return;
		}
		if(currentEndpoint &&
		        currentEndpoint->type != EP_TYPE_UDP_RELAY) {
			currentEndpoint = preferredRelay;
			for(std::vector<Endpoint *>::iterator itr =
			            endpoints.begin(); itr != endpoints.end();) {
				Endpoint *endpoint = *itr;
				if(endpoint->type == EP_TYPE_UDP_P2P_INET) {
					endpoint->averageRTT = 0;
					memset(endpoint->rtts, 0, sizeof(endpoint->rtts));
				}
				if(endpoint->type == EP_TYPE_UDP_P2P_LAN) {
					delete endpoint;
					itr = endpoints.erase(itr);
				} else {
					++itr;
				}
			}
		}
		if(allowP2p && currentEndpoint) {
			SendPublicEndpointsRequest();
		}
		BufferOutputStream s(4);
		s.WriteInt32(dataSavingMode ?
		             INIT_FLAG_DATA_SAVING_ENABLED : 0);
		SendPacketReliably(PKT_NETWORK_CHANGED,
		                   s.GetBuffer(), s.GetLength(), 1, 20);
	}
	LOGI("set network type: %d, active interface %s",
	     type, activeNetItfName.c_str());
	/*if(type==NET_TYPE_GPRS || type==NET_TYPE_EDGE)
		audioPacketGrouping=2;
	else
		audioPacketGrouping=1;*/
}


double VoIPController::GetAverageRTT()
{
	if(lastSentSeq >= lastRemoteAckSeq) {
		uint32_t diff = lastSentSeq - lastRemoteAckSeq;
		//LOGV("rtt diff=%u", diff);
		if(diff < 32) {
			int i;
			double res = 0;
			int count = 0;
			for(i = diff; i < 32; i++) {
				if(remoteAcks[i - diff] > 0) {
					res += (remoteAcks[i - diff] - sentPacketTimes[i]);
					count++;
				}
			}
			if(count > 0) {
				res /= count;
			}
			return res;
		}
	}
	return 999;
}

#if defined(__APPLE__)
static void initMachTimestart()
{
	mach_timebase_info_data_t tb = { 0, 0 };
	mach_timebase_info(&tb);
	VoIPController::machTimebase = tb.numer;
	VoIPController::machTimebase /= tb.denom;
	VoIPController::machTimestart =
	    mach_absolute_time();
}
#endif

double VoIPController::GetCurrentTime()
{
#if defined(__linux__)
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
#elif defined(__APPLE__)
	static pthread_once_t token = PTHREAD_ONCE_INIT;
	pthread_once(&token, &initMachTimestart);
	return (mach_absolute_time() - machTimestart) *
	       machTimebase / 1000000000.0f;
#elif defined(_WIN32)
	if(!didInitWin32TimeScale) {
		LARGE_INTEGER scale;
		QueryPerformanceFrequency(&scale);
		win32TimeScale = scale.QuadPart;
		didInitWin32TimeScale = true;
	}
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart / (double)win32TimeScale;
#endif
}

void VoIPController::SetStateCallback(void (* f)(
        VoIPController *, int))
{
	stateCallback = f;
	if(stateCallback) {
		stateCallback(this, state);
	}
}


void VoIPController::SetState(int state)
{
	this->state = state;
	LOGV("Call state changed to %d", state);
	stateChangeTime = GetCurrentTime();
	if(stateCallback) {
		stateCallback(this, state);
	}
}


void VoIPController::SetMicMute(bool mute)
{
	micMuted = mute;
	if(audioInput) {
		if(mute) {
			audioInput->Stop();
		} else {
			audioInput->Start();
		}
		if(!audioInput->IsInitialized()) {
			lastError = TGVOIP_ERROR_AUDIO_IO;
			SetState(STATE_FAILED);
			return;
		}
	}
	if(echoCanceller) {
		echoCanceller->Enable(!mute);
	}
	int i;
	for(i = 0; i < outgoingStreams.size(); i++) {
		if(outgoingStreams[i]->type == STREAM_TYPE_AUDIO) {
			unsigned char buf[2];
			buf[0] = outgoingStreams[i]->id;
			buf[1] = (char) (mute ? 0 : 1);
			SendPacketReliably(PKT_STREAM_STATE, buf, 2, .5f,
			                   20);
			outgoingStreams[i]->enabled = !mute;
		}
	}
}


void VoIPController::UpdateAudioOutputState()
{
	bool areAnyAudioStreamsEnabled = false;
	int i;
	for(i = 0; i < incomingStreams.size(); i++) {
		if(incomingStreams[i]->type == STREAM_TYPE_AUDIO &&
		        incomingStreams[i]->enabled) {
			areAnyAudioStreamsEnabled = true;
		}
	}
	if(jitterBuffer) {
		jitterBuffer->Reset();
	}
	if(decoder) {
		decoder->ResetQueue();
	}
	if(audioOutput) {
		if(audioOutput->IsPlaying() !=
		        areAnyAudioStreamsEnabled) {
			if(areAnyAudioStreamsEnabled) {
				audioOutput->Start();
			} else {
				audioOutput->Stop();
			}
		}
	}
}


BufferOutputStream
*VoIPController::GetOutgoingPacketBuffer()
{
	BufferOutputStream *pkt = NULL;
	lock_mutex(sendBufferMutex);
	if(emptySendBuffers.size() > 0) {
		pkt = emptySendBuffers[emptySendBuffers.size() - 1];
		emptySendBuffers.pop_back();
	}
	unlock_mutex(sendBufferMutex);
	return pkt;
}


void VoIPController::KDF(unsigned char *msgKey,
                         size_t x, unsigned char *aesKey,
                         unsigned char *aesIv)
{
	uint8_t sA[SHA1_LENGTH], sB[SHA1_LENGTH],
	        sC[SHA1_LENGTH], sD[SHA1_LENGTH];
	BufferOutputStream buf(128);
	buf.WriteBytes(msgKey, 16);
	buf.WriteBytes(encryptionKey + x, 32);
	crypto.sha1(buf.GetBuffer(), buf.GetLength(), sA);
	buf.Reset();
	buf.WriteBytes(encryptionKey + 32 + x, 16);
	buf.WriteBytes(msgKey, 16);
	buf.WriteBytes(encryptionKey + 48 + x, 16);
	crypto.sha1(buf.GetBuffer(), buf.GetLength(), sB);
	buf.Reset();
	buf.WriteBytes(encryptionKey + 64 + x, 32);
	buf.WriteBytes(msgKey, 16);
	crypto.sha1(buf.GetBuffer(), buf.GetLength(), sC);
	buf.Reset();
	buf.WriteBytes(msgKey, 16);
	buf.WriteBytes(encryptionKey + 96 + x, 32);
	crypto.sha1(buf.GetBuffer(), buf.GetLength(), sD);
	buf.Reset();
	buf.WriteBytes(sA, 8);
	buf.WriteBytes(sB + 8, 12);
	buf.WriteBytes(sC + 4, 12);
	assert(buf.GetLength() == 32);
	memcpy(aesKey, buf.GetBuffer(), 32);
	buf.Reset();
	buf.WriteBytes(sA + 8, 12);
	buf.WriteBytes(sB, 8);
	buf.WriteBytes(sC + 16, 4);
	buf.WriteBytes(sD, 8);
	assert(buf.GetLength() == 32);
	memcpy(aesIv, buf.GetBuffer(), 32);
}

void VoIPController::GetDebugString(char *buffer,
                                    size_t len)
{
	char endpointsBuf[10240];
	memset(endpointsBuf, 0, 10240);
	int i;
	for(std::vector<Endpoint *>::iterator itrtr =
	            endpoints.begin(); itrtr != endpoints.end();
	        ++itrtr) {
		const char *type;
		Endpoint *endpoint = *itrtr;
		switch(endpoint->type) {
		case EP_TYPE_UDP_P2P_INET:
			type = "UDP_P2P_INET";
			break;
		case EP_TYPE_UDP_P2P_LAN:
			type = "UDP_P2P_LAN";
			break;
		case EP_TYPE_UDP_RELAY:
			type = "UDP_RELAY";
			break;
		case EP_TYPE_TCP_RELAY:
			type = "TCP_RELAY";
			break;
		default:
			type = "UNKNOWN";
			break;
		}
		if(strlen(endpointsBuf) > 10240 - 1024) {
			break;
		}
		sprintf(endpointsBuf + strlen(endpointsBuf),
		        "%s:%u %dms [%s%s]\n",
		        endpoint->address.ToString().c_str(),
		        endpoint->port, (int)(endpoint->averageRTT * 1000),
		        type, currentEndpoint == endpoint ? ", IN_USE" :
		        "");
	}
	double avgLate[3];
	if(jitterBuffer) {
		jitterBuffer->GetAverageLateCount(avgLate);
	} else {
		memset(avgLate, 0, 3 * sizeof(double));
	}
	snprintf(buffer, len,
	         "Remote endpoints: \n%s"
	         "Jitter buffer: %d/%d | %.1f, %.1f, %.1f\n"
	         "RTT avg/min: %d/%d\n"
	         "Congestion window: %d/%d bytes\n"
	         "Key fingerprint: %02hhX%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX\n"
	         "Last sent/ack'd seq: %u/%u\n"
	         "Last recvd seq: %u\n"
	         "Send/recv losses: %u/%u (%d%%)\n"
	         "Audio bitrate: %d kbit\n"
//					 "Packet grouping: %d\n"
	         "Frame size out/in: %d/%d\n"
	         "Bytes sent/recvd: %llu/%llu",
	         endpointsBuf,
	         jitterBuffer ? jitterBuffer->GetMinPacketCount() :
	         0, jitterBuffer ? jitterBuffer->GetCurrentDelay()
	         : 0, avgLate[0], avgLate[1], avgLate[2],
	         // (int)(GetAverageRTT()*1000), 0,
	         (int)(conctl->GetAverageRTT() * 1000),
	         (int)(conctl->GetMinimumRTT() * 1000),
	         int(conctl->GetInflightDataSize()),
	         int(conctl->GetCongestionWindow()),
	         keyFingerprint[0], keyFingerprint[1],
	         keyFingerprint[2], keyFingerprint[3],
	         keyFingerprint[4], keyFingerprint[5],
	         keyFingerprint[6], keyFingerprint[7],
	         lastSentSeq, lastRemoteAckSeq, lastRemoteSeq,
	         conctl->GetSendLossCount(), recvLossCount,
	         encoder ? encoder->GetPacketLoss() : 0,
	         encoder ? (encoder->GetBitrate() / 1000) : 0,
//			 audioPacketGrouping,
	         outgoingStreams[0]->frameDuration,
	         incomingStreams.size() > 0 ?
	         incomingStreams[0]->frameDuration : 0,
	         (long long unsigned int)(stats.bytesSentMobile
	                                  + stats.bytesSentWifi),
	         (long long unsigned int)(stats.bytesRecvdMobile
	                                  + stats.bytesRecvdWifi));
}


void VoIPController::SendPublicEndpointsRequest()
{
	LOGI("Sending public endpoints request");
	if(preferredRelay) {
		SendPublicEndpointsRequest(*preferredRelay);
	}
	if(peerPreferredRelay &&
	        peerPreferredRelay != preferredRelay) {
		SendPublicEndpointsRequest(*peerPreferredRelay);
	}
}

void VoIPController::SendPublicEndpointsRequest(
    Endpoint &relay)
{
	LOGD("Sending public endpoints request to %s:%d",
	     relay.address.ToString().c_str(), relay.port);
	publicEndpointsReqTime = GetCurrentTime();
	waitingForRelayPeerInfo = true;
	unsigned char buf[32];
	memcpy(buf, relay.peerTag, 16);
	memset(buf + 16, 0xFF, 16);
	NetworkPacket pkt;
	pkt.data = buf;
	pkt.length = 32;
	pkt.address = (NetworkAddress *)&relay.address;
	pkt.port = relay.port;
	socket->Send(&pkt);
}

Endpoint *VoIPController::GetEndpointByType(
    int type)
{
	if(type == EP_TYPE_UDP_RELAY && preferredRelay) {
		return preferredRelay;
	}
	for(std::vector<Endpoint *>::iterator itrtr =
	            endpoints.begin(); itrtr != endpoints.end();
	        ++itrtr) {
		if((*itrtr)->type == type) {
			return *itrtr;
		}
	}
	return NULL;
}


float VoIPController::GetOutputLevel()
{
	if(!audioOutput || !audioOutStarted) {
		return 0.0;
	}
	return audioOutput->GetLevel();
}


void VoIPController::SendPacketReliably(
    unsigned char type, unsigned char *data,
    size_t len, double retryInterval, double timeout)
{
	LOGD("Send reliably, type=%u, len=%u, retry=%.3f, timeout=%.3f",
	     type, unsigned(len), retryInterval, timeout);
	voip_queued_packet_t *pkt = (voip_queued_packet_t *)
	                            malloc(sizeof(voip_queued_packet_t));
	memset(pkt, 0, sizeof(voip_queued_packet_t));
	pkt->type = type;
	if(data) {
		pkt->data = (unsigned char *) malloc(len);
		memcpy(pkt->data, data, len);
		pkt->length = len;
	}
	pkt->retryInterval = retryInterval;
	pkt->timeout = timeout;
	pkt->firstSentTime = 0;
	pkt->lastSentTime = 0;
	lock_mutex(queuedPacketsMutex);
	queuedPackets.push_back(pkt);
	unlock_mutex(queuedPacketsMutex);
}


void VoIPController::SetConfig(voip_config_t *cfg)
{
	memcpy(&config, cfg, sizeof(voip_config_t));
	if(tgvoipLogFile) {
		fclose(tgvoipLogFile);
	}
	if(strlen(cfg->logFilePath)) {
		tgvoipLogFile = fopen(cfg->logFilePath, "a");
		tgvoip_log_file_write_header();
	}
	if(statsDump) {
		fclose(statsDump);
	}
	if(strlen(cfg->statsDumpFilePath)) {
		statsDump = fopen(cfg->statsDumpFilePath, "w");
		fprintf(statsDump,
		        "Time\tRTT\tLRSeq\tLSSeq\tLASeq\tLostR\tLostS\tCWnd\tBitrate\tLoss%%\tJitter\tJDelay\tAJDelay\n");
	}
	UpdateDataSavingState();
	UpdateAudioBitrate();
}


void VoIPController::UpdateDataSavingState()
{
	if(config.data_saving == DATA_SAVING_ALWAYS) {
		dataSavingMode = true;
	} else if(config.data_saving ==
	          DATA_SAVING_MOBILE) {
		dataSavingMode = networkType == NET_TYPE_GPRS ||
		                 networkType == NET_TYPE_EDGE ||
		                 networkType == NET_TYPE_3G ||
		                 networkType == NET_TYPE_HSPA ||
		                 networkType == NET_TYPE_LTE ||
		                 networkType == NET_TYPE_OTHER_MOBILE;
	} else {
		dataSavingMode = false;
	}
	LOGI("update data saving mode, config %d, enabled %d, reqd by peer %d",
	     config.data_saving, dataSavingMode,
	     dataSavingRequestedByPeer);
}


void VoIPController::DebugCtl(int request,
                              int param)
{
	if(request == 1) { // set bitrate
		maxBitrate = param;
		if(encoder) {
			encoder->SetBitrate(maxBitrate);
		}
	} else if(request == 2) { // set packet loss
		if(encoder) {
			encoder->SetPacketLoss(param);
		}
	} else if(request ==
	          3) { // force enable/disable p2p
		allowP2p = param == 1;
		if(!allowP2p && currentEndpoint &&
		        currentEndpoint->type != EP_TYPE_UDP_RELAY) {
			currentEndpoint = preferredRelay;
		} else if(allowP2p) {
			SendPublicEndpointsRequest();
		}
		BufferOutputStream s(4);
		s.WriteInt32(dataSavingMode ?
		             INIT_FLAG_DATA_SAVING_ENABLED : 0);
		SendPacketReliably(PKT_NETWORK_CHANGED,
		                   s.GetBuffer(), s.GetLength(), 1, 20);
	} else if(request == 4) {
		if(echoCanceller) {
			echoCanceller->Enable(param == 1);
		}
	}
}


const char *VoIPController::GetVersion()
{
	return LIBTGVOIP_VERSION;
}


int64_t VoIPController::GetPreferredRelayID()
{
	if(preferredRelay) {
		return preferredRelay->id;
	}
	return 0;
}


int VoIPController::GetLastError()
{
	return lastError;
}


void VoIPController::GetStats(voip_stats_t *stats)
{
	memcpy(stats, &this->stats, sizeof(voip_stats_t));
}

#ifdef TGVOIP_USE_AUDIO_SESSION
void VoIPController::SetAcquireAudioSession(void (
            ^completion)(void (^)()))
{
	this->acquireAudioSession = [completion copy];
}

void VoIPController::ReleaseAudioSession(void (
            ^completion)())
{
	completion();
}
#endif

void VoIPController::LogDebugInfo()
{
	std::string json = "{\"endpoints\":[";
	for(std::vector<Endpoint *>::iterator itr =
	            endpoints.begin(); itr != endpoints.end(); ++itr) {
		Endpoint *e = *itr;
		char buffer[1024];
		const char *typeStr = "unknown";
		switch(e->type) {
		case EP_TYPE_UDP_RELAY:
			typeStr = "udp_relay";
			break;
		case EP_TYPE_UDP_P2P_INET:
			typeStr = "udp_p2p_inet";
			break;
		case EP_TYPE_UDP_P2P_LAN:
			typeStr = "udp_p2p_lan";
			break;
		}
		snprintf(buffer, 1024,
		         "{\"address\":\"%s\",\"port\":%u,\"type\":\"%s\",\"rtt\":%u%s%s}",
		         e->address.ToString().c_str(), e->port, typeStr,
		         (unsigned int)round(e->averageRTT * 1000),
		         currentEndpoint == &*e ? ",\"in_use\":true" : "",
		         preferredRelay == &*e ? ",\"preferred\":true" : "");
		json += buffer;
		if(itr != endpoints.end() - 1) {
			json += ",";
		}
	}
	json += "],";
	char buffer[1024];
	const char *netTypeStr;
	switch(networkType) {
	case NET_TYPE_WIFI:
		netTypeStr = "wifi";
		break;
	case NET_TYPE_GPRS:
		netTypeStr = "gprs";
		break;
	case NET_TYPE_EDGE:
		netTypeStr = "edge";
		break;
	case NET_TYPE_3G:
		netTypeStr = "3g";
		break;
	case NET_TYPE_HSPA:
		netTypeStr = "hspa";
		break;
	case NET_TYPE_LTE:
		netTypeStr = "lte";
		break;
	case NET_TYPE_ETHERNET:
		netTypeStr = "ethernet";
		break;
	case NET_TYPE_OTHER_HIGH_SPEED:
		netTypeStr = "other_high_speed";
		break;
	case NET_TYPE_OTHER_LOW_SPEED:
		netTypeStr = "other_low_speed";
		break;
	case NET_TYPE_DIALUP:
		netTypeStr = "dialup";
		break;
	case NET_TYPE_OTHER_MOBILE:
		netTypeStr = "other_mobile";
		break;
	default:
		netTypeStr = "unknown";
		break;
	}
	snprintf(buffer, 1024,
	         "\"time\":%u,\"network_type\":\"%s\"}",
	         (unsigned int)time(NULL), netTypeStr);
	json += buffer;
	debugLogs.push_back(json);
}

std::string VoIPController::GetDebugLog()
{
	std::string log = "{\"events\":[";

	for(std::vector<std::string>::iterator itr =
	            debugLogs.begin(); itr != debugLogs.end(); ++itr) {
		log += (*itr);
		if((itr + 1) != debugLogs.end()) {
			log += ",";
		}
	}
	log += "],\"libtgvoip_version\":\""
	       LIBTGVOIP_VERSION "\"}";
	return log;
}

void VoIPController::GetDebugLog(char *buffer)
{
	strcpy(buffer, GetDebugLog().c_str());
}

size_t VoIPController::GetDebugLogLength()
{
	size_t len = 128;
	for(std::vector<std::string>::iterator itr =
	            debugLogs.begin(); itr != debugLogs.end(); ++itr) {
		len += (*itr).length() + 1;
	}
	return len;
}

std::vector<AudioInputDevice>
VoIPController::EnumerateAudioInputs()
{
	vector<AudioInputDevice> devs;
	audio::AudioInput::EnumerateDevices(devs);
	return devs;
}

std::vector<AudioOutputDevice>
VoIPController::EnumerateAudioOutputs()
{
	vector<AudioOutputDevice> devs;
	audio::AudioOutput::EnumerateDevices(devs);
	return devs;
}

void VoIPController::SetCurrentAudioInput(
    std::string id)
{
	currentAudioInput = id;
	if(audioInput) {
		audioInput->SetCurrentDevice(id);
	}
}

void VoIPController::SetCurrentAudioOutput(
    std::string id)
{
	currentAudioOutput = id;
	if(audioOutput) {
		audioOutput->SetCurrentDevice(id);
	}
}

std::string
VoIPController::GetCurrentAudioInputID()
{
	return currentAudioInput;
}

std::string
VoIPController::GetCurrentAudioOutputID()
{
	return currentAudioOutput;
}

Endpoint::Endpoint(int64_t id, uint16_t port,
                   IPv4Address &_address, IPv6Address &_v6address,
                   char type, unsigned char peerTag[16]) : address(
	                       _address), v6address(_v6address)
{
	this->id = id;
	this->port = port;
	this->type = type;
	memcpy(this->peerTag, peerTag, 16);
	LOGV("new endpoint %lld: %s:%u",
	     (long long int)id, address.ToString().c_str(),
	     port);

	lastPingSeq = 0;
	lastPingTime = 0;
	averageRTT = 0;
	memset(rtts, 0, sizeof(rtts));
}

Endpoint::Endpoint() : address(0),
	v6address("::0")
{
	lastPingSeq = 0;
	lastPingTime = 0;
	averageRTT = 0;
	memset(rtts, 0, sizeof(rtts));
}

#if defined(__APPLE__) && TARGET_OS_IPHONE
void VoIPController::SetRemoteEndpoints(
    voip_legacy_endpoint_t *buffer, size_t count,
    bool allowP2P)
{
	std::vector<Endpoint> endpoints;
	for(size_t i = 0; i < count; i++) {
		voip_legacy_endpoint_t e = buffer[i];
		IPv4Address v4addr = IPv4Address(std::string(
		                                     e.address));
		IPv6Address v6addr = IPv6Address(std::string(
		                                     e.address6));
		endpoints.push_back(Endpoint(e.id, e.port, v4addr,
		                             v6addr, EP_TYPE_UDP_RELAY, e.peerTag));
	}
	this->SetRemoteEndpoints(endpoints, allowP2P);
}
#endif
