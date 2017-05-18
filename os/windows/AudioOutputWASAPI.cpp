//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//


#include <assert.h>
#include "AudioOutputWASAPI.h"
#include "../../logging.h"
#include "../../VoIPController.h"

#define BUFFER_SIZE 960
#define CHECK_RES(res, msg) {if(FAILED(res)){LOGE("%s failed: HRESULT=0x%08X", msg, res); failed=true; return;}}
#define SCHECK_RES(res, msg) {if(FAILED(res)){LOGE("%s failed: HRESULT=0x%08X", msg, res); return;}}

template <class T> void SafeRelease(T **ppT)
{
	if(*ppT) {
		(*ppT)->Release();
		*ppT = NULL;
	}
}

#ifdef TGVOIP_WINXP_COMPAT

#endif

using namespace tgvoip::audio;

AudioOutputWASAPI::AudioOutputWASAPI(
    std::string deviceID)
{
	isPlaying = false;
	remainingDataLen = 0;
	refCount = 1;
	HRESULT res;
	res = CoInitializeEx(NULL,
	                     COINIT_APARTMENTTHREADED);
	CHECK_RES(res, "CoInitializeEx");
#ifdef TGVOIP_WINXP_COMPAT
	HANDLE (WINAPI * __CreateEventExA)(
	    LPSECURITY_ATTRIBUTES lpEventAttributes,
	    LPCSTR lpName, DWORD dwFlags,
	    DWORD dwDesiredAccess);
	__CreateEventExA = (HANDLE (WINAPI *)(
	                        LPSECURITY_ATTRIBUTES, LPCSTR, DWORD,
	                        DWORD))GetProcAddress(
	                       GetModuleHandleA("kernel32.dll"),
	                       "CreateEventExA");
#undef CreateEventEx
#define CreateEventEx __CreateEventExA
#endif
	shutdownEvent = CreateEventEx(NULL, NULL, 0,
	                              EVENT_MODIFY_STATE | SYNCHRONIZE);
	audioSamplesReadyEvent = CreateEventEx(NULL, NULL,
	                                       0, EVENT_MODIFY_STATE | SYNCHRONIZE);
	streamSwitchEvent = CreateEventEx(NULL, NULL, 0,
	                                  EVENT_MODIFY_STATE | SYNCHRONIZE);
	ZeroMemory(&format, sizeof(format));
	format.wFormatTag = WAVE_FORMAT_PCM;
	format.nChannels = 1;
	format.nSamplesPerSec = 48000;
	format.nBlockAlign = 2;
	format.nAvgBytesPerSec =
	    format.nSamplesPerSec * format.nBlockAlign;
	format.wBitsPerSample = 16;

#ifdef TGVOIP_WINDOWS_DESKTOP
	res = CoCreateInstance(__uuidof(MMDeviceEnumerator),
	                       NULL, CLSCTX_INPROC_SERVER,
	                       IID_PPV_ARGS(&enumerator));
	CHECK_RES(res,
	          "CoCreateInstance(MMDeviceEnumerator)");
	res = enumerator->RegisterEndpointNotificationCallback(
	          this);
	CHECK_RES(res,
	          "enumerator->RegisterEndpointNotificationCallback");
	audioSessionControl = NULL;
	device = NULL;
#endif

	audioClient = NULL;
	renderClient = NULL;
	thread = NULL;

	SetCurrentDevice(deviceID);
}

AudioOutputWASAPI::~AudioOutputWASAPI()
{
	if(audioClient && isPlaying) {
		audioClient->Stop();
	}

#ifdef TGVOIP_WINDOWS_DESKTOP
	if(audioSessionControl) {
		audioSessionControl->UnregisterAudioSessionNotification(
		    this);
	}
#endif

	SetEvent(shutdownEvent);
	if(thread) {
		WaitForSingleObjectEx(thread, INFINITE, false);
		CloseHandle(thread);
	}
	SafeRelease(&renderClient);
	SafeRelease(&audioClient);
#ifdef TGVOIP_WINDOWS_DESKTOP
	SafeRelease(&device);
	SafeRelease(&audioSessionControl);
#endif
	CloseHandle(shutdownEvent);
	CloseHandle(audioSamplesReadyEvent);
	CloseHandle(streamSwitchEvent);
#ifdef TGVOIP_WINDOWS_DESKTOP
	if(enumerator) {
		enumerator->UnregisterEndpointNotificationCallback(
		    this);
	}
	SafeRelease(&enumerator);
#endif
}

void AudioOutputWASAPI::Configure(uint32_t
                                  sampleRate, uint32_t bitsPerSample,
                                  uint32_t channels)
{

}

void AudioOutputWASAPI::Start()
{
	isPlaying = true;
	if(!thread) {
		thread = CreateThread(NULL, 0,
		                      (LPTHREAD_START_ROUTINE)
		                      AudioOutputWASAPI::StartThread, this, 0, NULL);
	}

	if(audioClient) {
		audioClient->Start();
	}
}

void AudioOutputWASAPI::Stop()
{
	isPlaying = false;

	if(audioClient) {
		audioClient->Stop();
	}
}

bool AudioOutputWASAPI::IsPlaying()
{
	return isPlaying;
}

void AudioOutputWASAPI::EnumerateDevices(
    std::vector<tgvoip::AudioOutputDevice> &devs)
{
#ifdef TGVOIP_WINDOWS_DESKTOP
	HRESULT res;
	res = CoInitializeEx(NULL,
	                     COINIT_APARTMENTTHREADED);
	SCHECK_RES(res, "CoInitializeEx");

	IMMDeviceEnumerator *deviceEnumerator = NULL;
	IMMDeviceCollection *deviceCollection = NULL;

	res = CoCreateInstance(__uuidof(MMDeviceEnumerator),
	                       NULL, CLSCTX_INPROC_SERVER,
	                       IID_PPV_ARGS(&deviceEnumerator));
	SCHECK_RES(res,
	           "CoCreateInstance(MMDeviceEnumerator)");

	res = deviceEnumerator->EnumAudioEndpoints(eRender,
	        DEVICE_STATE_ACTIVE, &deviceCollection);
	SCHECK_RES(res, "EnumAudioEndpoints");

	UINT devCount;
	res = deviceCollection->GetCount(&devCount);
	SCHECK_RES(res, "GetCount");

	for(UINT i = 0; i < devCount; i++) {
		IMMDevice *device;
		res = deviceCollection->Item(i, &device);
		SCHECK_RES(res, "GetDeviceItem");
		wchar_t *devID;
		res = device->GetId(&devID);
		SCHECK_RES(res, "get device id");

		IPropertyStore *propStore;
		res = device->OpenPropertyStore(STGM_READ,
		                                &propStore);
		SafeRelease(&device);
		SCHECK_RES(res, "OpenPropertyStore");

		PROPVARIANT friendlyName;
		PropVariantInit(&friendlyName);
		res = propStore->GetValue(PKEY_Device_FriendlyName,
		                          &friendlyName);
		SafeRelease(&propStore);

		AudioOutputDevice dev;

		wchar_t actualFriendlyName[128];
		if(friendlyName.vt == VT_LPWSTR) {
			wcsncpy(actualFriendlyName, friendlyName.pwszVal,
			        sizeof(actualFriendlyName) / sizeof(wchar_t));
		} else {
			wcscpy(actualFriendlyName, L"Unknown");
		}
		PropVariantClear(&friendlyName);

		char buf[256];
		WideCharToMultiByte(CP_UTF8, 0, devID, -1, buf,
		                    sizeof(buf), NULL, NULL);
		dev.id = buf;
		WideCharToMultiByte(CP_UTF8, 0,
		                    actualFriendlyName, -1, buf, sizeof(buf), NULL,
		                    NULL);
		dev.displayName = buf;
		devs.push_back(dev);

		CoTaskMemFree(devID);
	}

	SafeRelease(&deviceCollection);
	SafeRelease(&deviceEnumerator);
#endif
}

void AudioOutputWASAPI::SetCurrentDevice(
    std::string deviceID)
{
	if(thread) {
		streamChangeToDevice = deviceID;
		SetEvent(streamSwitchEvent);
	} else {
		ActuallySetCurrentDevice(deviceID);
	}
}

void AudioOutputWASAPI::ActuallySetCurrentDevice(
    std::string deviceID)
{
	currentDevice = deviceID;
	HRESULT res;

	if(audioClient && isPlaying) {
		res = audioClient->Stop();
		CHECK_RES(res, "audioClient->Stop");
	}

#ifdef TGVOIP_WINDOWS_DESKTOP
	if(audioSessionControl) {
		res = audioSessionControl->UnregisterAudioSessionNotification(
		          this);
		CHECK_RES(res,
		          "audioSessionControl->UnregisterAudioSessionNotification");
	}

	SafeRelease(&audioSessionControl);
#endif
	SafeRelease(&renderClient);
	SafeRelease(&audioClient);
#ifdef TGVOIP_WINDOWS_DESKTOP
	SafeRelease(&device);


	IMMDeviceCollection *deviceCollection = NULL;

	if(deviceID == "default") {
		isDefaultDevice = true;
		res = enumerator->GetDefaultAudioEndpoint(eRender,
		        eCommunications, &device);
		CHECK_RES(res, "GetDefaultAudioEndpoint");
	} else {
		isDefaultDevice = false;
		res = enumerator->EnumAudioEndpoints(eRender,
		                                     DEVICE_STATE_ACTIVE, &deviceCollection);
		CHECK_RES(res, "EnumAudioEndpoints");

		UINT devCount;
		res = deviceCollection->GetCount(&devCount);
		CHECK_RES(res, "GetCount");

		for(UINT i = 0; i < devCount; i++) {
			IMMDevice *device;
			res = deviceCollection->Item(i, &device);
			CHECK_RES(res, "GetDeviceItem");
			wchar_t *_devID;
			res = device->GetId(&_devID);
			CHECK_RES(res, "get device id");

			char devID[128];
			WideCharToMultiByte(CP_UTF8, 0, _devID, -1, devID,
			                    128, NULL, NULL);

			CoTaskMemFree(_devID);
			if(deviceID == devID) {
				this->device = device;
				break;
			}
		}
	}

	if(deviceCollection) {
		SafeRelease(&deviceCollection);
	}

	if(!device) {
		LOGE("Didn't find playback device; failing");
		failed = true;
		return;
	}

	res = device->Activate(__uuidof(IAudioClient),
	                       CLSCTX_INPROC_SERVER, NULL, (void **)&audioClient);
	CHECK_RES(res, "device->Activate");
#else
	Platform::String ^defaultDevID =
	    Windows::Media::Devices::MediaDevice::GetDefaultAudioRenderId(
	        Windows::Media::Devices::AudioDeviceRole::Communications);
	HRESULT res1, res2;
	audioClient =
	    WindowsSandboxUtils::ActivateAudioDevice(
	        defaultDevID->Data(), &res1, &res2);
	CHECK_RES(res1, "activate1");
	CHECK_RES(res2, "activate2");
#endif

	res = audioClient->Initialize(
	          AUDCLNT_SHAREMODE_SHARED,
	          AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
	          AUDCLNT_STREAMFLAGS_NOPERSIST |
	          0x80000000/*AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM*/,
	          60 * 10000, 0, &format, NULL);
	CHECK_RES(res, "audioClient->Initialize");

	uint32_t bufSize;
	res = audioClient->GetBufferSize(&bufSize);
	CHECK_RES(res, "audioClient->GetBufferSize");

	LOGV("buffer size: %u", bufSize);

	res = audioClient->SetEventHandle(
	          audioSamplesReadyEvent);
	CHECK_RES(res, "audioClient->SetEventHandle");

	res = audioClient->GetService(IID_PPV_ARGS(
	                                  &renderClient));
	CHECK_RES(res, "audioClient->GetService");

	BYTE *data;
	res = renderClient->GetBuffer(bufSize, &data);
	CHECK_RES(res, "renderClient->GetBuffer");

	res = renderClient->ReleaseBuffer(bufSize,
	                                  AUDCLNT_BUFFERFLAGS_SILENT);
	CHECK_RES(res, "renderClient->ReleaseBuffer");

#ifdef TGVOIP_WINDOWS_DESKTOP
	res = audioClient->GetService(IID_PPV_ARGS(
	                                  &audioSessionControl));
	CHECK_RES(res,
	          "audioClient->GetService(IAudioSessionControl)");

	res = audioSessionControl->RegisterAudioSessionNotification(
	          this);
	CHECK_RES(res,
	          "audioSessionControl->RegisterAudioSessionNotification");
#endif

	if(isPlaying) {
		audioClient->Start();
	}

	LOGV("set current output device done");
}

DWORD AudioOutputWASAPI::StartThread(void *arg)
{
	((AudioOutputWASAPI *)arg)->RunThread();
	return 0;
}

void AudioOutputWASAPI::RunThread()
{
	SetThreadPriority(GetCurrentThread(),
	                  THREAD_PRIORITY_HIGHEST);

	HANDLE waitArray[] = {shutdownEvent, streamSwitchEvent, audioSamplesReadyEvent};
	HRESULT res = CoInitializeEx(NULL,
	                             COINIT_APARTMENTTHREADED);
	CHECK_RES(res, "CoInitializeEx in render thread");

	uint32_t bufferSize;
	res = audioClient->GetBufferSize(&bufferSize);
	CHECK_RES(res, "audioClient->GetBufferSize");
	uint32_t framesWritten = 0;

	bool running = true;

	while(running) {
		DWORD waitResult = WaitForMultipleObjectsEx(3,
		                   waitArray, false, INFINITE, false);
		if(waitResult == WAIT_OBJECT_0) { // shutdownEvent
			LOGV("render thread shutting down");
			running = false;
		} else if(waitResult == WAIT_OBJECT_0
		          + 1) { // streamSwitchEvent
			LOGV("stream switch");
			ActuallySetCurrentDevice(streamChangeToDevice);
			ResetEvent(streamSwitchEvent);
			LOGV("stream switch done");
		} else if(waitResult == WAIT_OBJECT_0
		          + 2) { // audioSamplesReadyEvent
			if(!audioClient) {
				continue;
			}
			BYTE *data;
			uint32_t padding;
			uint32_t framesAvailable;
			res = audioClient->GetCurrentPadding(&padding);
			CHECK_RES(res, "audioClient->GetCurrentPadding");
			framesAvailable = bufferSize - padding;
			res = renderClient->GetBuffer(framesAvailable,
			                              &data);
			CHECK_RES(res, "renderClient->GetBuffer");

			size_t bufferSize = framesAvailable * 2;
			while(bufferSize > remainingDataLen) {
				InvokeCallback(remainingData + remainingDataLen,
				               960 * 2);
				remainingDataLen += 960 * 2;
			}
			memcpy(data, remainingData, bufferSize);
			if(remainingDataLen > bufferSize) {
				memmove(remainingData, remainingData + bufferSize,
				        remainingDataLen - bufferSize);
			}
			remainingDataLen -= bufferSize;

			res = renderClient->ReleaseBuffer(framesAvailable,
			                                  0);
			CHECK_RES(res, "renderClient->ReleaseBuffer");
			framesWritten += framesAvailable;
		}
	}
}

#ifdef TGVOIP_WINDOWS_DESKTOP
HRESULT AudioOutputWASAPI::OnSessionDisconnected(
    AudioSessionDisconnectReason reason)
{
	if(!isDefaultDevice) {
		streamChangeToDevice = "default";
		SetEvent(streamSwitchEvent);
	}
	return S_OK;
}

HRESULT AudioOutputWASAPI::OnDefaultDeviceChanged(
    EDataFlow flow, ERole role, LPCWSTR newDevID)
{
	if(flow == eRender && role == eCommunications &&
	        isDefaultDevice) {
		streamChangeToDevice = "default";
		SetEvent(streamSwitchEvent);
	}
	return S_OK;
}

ULONG AudioOutputWASAPI::AddRef()
{
	return InterlockedIncrement(&refCount);
}

ULONG AudioOutputWASAPI::Release()
{
	return InterlockedDecrement(&refCount);
}

HRESULT AudioOutputWASAPI::QueryInterface(
    REFIID iid, void **obj)
{
	if(!obj) {
		return E_POINTER;
	}
	*obj = NULL;

	if(iid == IID_IUnknown) {
		*obj = static_cast<IUnknown *>
		       (static_cast<IAudioSessionEvents *>(this));
		AddRef();
	} else if(iid == __uuidof(IMMNotificationClient)) {
		*obj = static_cast<IMMNotificationClient *>(this);
		AddRef();
	} else if(iid == __uuidof(IAudioSessionEvents)) {
		*obj = static_cast<IAudioSessionEvents *>(this);
		AddRef();
	} else {
		return E_NOINTERFACE;
	}

	return S_OK;
}
#endif