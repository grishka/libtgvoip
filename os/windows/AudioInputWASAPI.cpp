//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//


#include <assert.h>
#include "AudioInputWASAPI.h"
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

using namespace tgvoip::audio;

AudioInputWASAPI::AudioInputWASAPI(
    std::string deviceID)
{
	isRecording = false;
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
	captureClient = NULL;
	thread = NULL;

	SetCurrentDevice(deviceID);
}

AudioInputWASAPI::~AudioInputWASAPI()
{
	if(audioClient && isRecording) {
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
#ifdef TGVOIP_WINDOWS_DESKTOP
	SafeRelease(&audioSessionControl);
#endif
	SafeRelease(&captureClient);
	SafeRelease(&audioClient);
#ifdef TGVOIP_WINDOWS_DESKTOP
	SafeRelease(&device);
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

void AudioInputWASAPI::Configure(uint32_t
                                 sampleRate, uint32_t bitsPerSample,
                                 uint32_t channels)
{

}

void AudioInputWASAPI::Start()
{
	isRecording = true;
	if(!thread) {
		thread = CreateThread(NULL, 0,
		                      (LPTHREAD_START_ROUTINE)
		                      AudioInputWASAPI::StartThread, this, 0, NULL);
	}

	if(audioClient) {
		audioClient->Start();
	}
}

void AudioInputWASAPI::Stop()
{
	isRecording = false;

	if(audioClient) {
		audioClient->Stop();
	}
}

bool AudioInputWASAPI::IsRecording()
{
	return isRecording;
}

void AudioInputWASAPI::EnumerateDevices(
    std::vector<tgvoip::AudioInputDevice> &devs)
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

	res = deviceEnumerator->EnumAudioEndpoints(eCapture,
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

		AudioInputDevice dev;

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

void AudioInputWASAPI::SetCurrentDevice(
    std::string deviceID)
{
	if(thread) {
		streamChangeToDevice = deviceID;
		SetEvent(streamSwitchEvent);
	} else {
		ActuallySetCurrentDevice(deviceID);
	}
}

void AudioInputWASAPI::ActuallySetCurrentDevice(
    std::string deviceID)
{
	currentDevice = deviceID;
	HRESULT res;

	if(audioClient) {
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
	SafeRelease(&captureClient);
	SafeRelease(&audioClient);
#ifdef TGVOIP_WINDOWS_DESKTOP
	SafeRelease(&device);

	IMMDeviceCollection *deviceCollection = NULL;

	if(deviceID == "default") {
		isDefaultDevice = true;
		res = enumerator->GetDefaultAudioEndpoint(eCapture,
		        eCommunications, &device);
		CHECK_RES(res, "GetDefaultAudioEndpoint");
	} else {
		isDefaultDevice = false;
		res = enumerator->EnumAudioEndpoints(eCapture,
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
				//device->AddRef();
				break;
			}
		}
	}

	if(deviceCollection) {
		SafeRelease(&deviceCollection);
	}

	if(!device) {
		LOGE("Didn't find capture device; failing");
		failed = true;
		return;
	}

	res = device->Activate(__uuidof(IAudioClient),
	                       CLSCTX_INPROC_SERVER, NULL, (void **)&audioClient);
	CHECK_RES(res, "device->Activate");
#else
	Platform::String ^defaultDevID =
	    Windows::Media::Devices::MediaDevice::GetDefaultAudioCaptureId(
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
	                                  &captureClient));
	CHECK_RES(res, "audioClient->GetService");

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

	if(isRecording) {
		audioClient->Start();
	}

	LOGV("set current output device done");
}

DWORD AudioInputWASAPI::StartThread(void *arg)
{
	((AudioInputWASAPI *)arg)->RunThread();
	return 0;
}

void AudioInputWASAPI::RunThread()
{
	SetThreadPriority(GetCurrentThread(),
	                  THREAD_PRIORITY_HIGHEST);

	HANDLE waitArray[] = {shutdownEvent, streamSwitchEvent, audioSamplesReadyEvent};
	HRESULT res = CoInitializeEx(NULL,
	                             COINIT_APARTMENTTHREADED);
	CHECK_RES(res,
	          "CoInitializeEx in capture thread");

	uint32_t bufferSize = 0;
	uint32_t framesWritten = 0;

	bool running = true;

	while(running) {
		DWORD waitResult = WaitForMultipleObjectsEx(3,
		                   waitArray, false, INFINITE, false);
		if(waitResult == WAIT_OBJECT_0) { // shutdownEvent
			LOGV("capture thread shutting down");
			running = false;
		} else if(waitResult == WAIT_OBJECT_0
		          + 1) { // streamSwitchEvent
			LOGV("stream switch");
			ActuallySetCurrentDevice(streamChangeToDevice);
			ResetEvent(streamSwitchEvent);
			bufferSize = 0;
			LOGV("stream switch done");
		} else if(waitResult == WAIT_OBJECT_0
		          + 2) { // audioSamplesReadyEvent
			if(!audioClient) {
				continue;
			}
			if(bufferSize == 0) {
				res = audioClient->GetBufferSize(&bufferSize);
				CHECK_RES(res, "audioClient->GetBufferSize");
			}
			BYTE *data;
			uint32_t framesAvailable;
			DWORD flags;

			res = captureClient->GetBuffer(&data,
			                               &framesAvailable, &flags, NULL, NULL);
			CHECK_RES(res, "captureClient->GetBuffer");
			size_t dataLen = framesAvailable * 2;
			assert(remainingDataLen + dataLen < sizeof(
			           remainingData));

			memcpy(remainingData + remainingDataLen, data,
			       dataLen);
			remainingDataLen += dataLen;
			while(remainingDataLen > 960 * 2) {
				InvokeCallback(remainingData, 960 * 2);
				memmove(remainingData, remainingData + (960 * 2),
				        remainingDataLen - 960 * 2);
				remainingDataLen -= 960 * 2;
			}

			res = captureClient->ReleaseBuffer(framesAvailable);
			CHECK_RES(res, "captureClient->ReleaseBuffer");

			framesWritten += framesAvailable;
		}
	}
}

#ifdef TGVOIP_WINDOWS_DESKTOP
HRESULT AudioInputWASAPI::OnSessionDisconnected(
    AudioSessionDisconnectReason reason)
{
	if(!isDefaultDevice) {
		streamChangeToDevice = "default";
		SetEvent(streamSwitchEvent);
	}
	return S_OK;
}

HRESULT AudioInputWASAPI::OnDefaultDeviceChanged(
    EDataFlow flow, ERole role, LPCWSTR newDevID)
{
	if(flow == eCapture && role == eCommunications &&
	        isDefaultDevice) {
		streamChangeToDevice = "default";
		SetEvent(streamSwitchEvent);
	}
	return S_OK;
}

ULONG AudioInputWASAPI::AddRef()
{
	return InterlockedIncrement(&refCount);
}

ULONG AudioInputWASAPI::Release()
{
	return InterlockedDecrement(&refCount);
}

HRESULT AudioInputWASAPI::QueryInterface(
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