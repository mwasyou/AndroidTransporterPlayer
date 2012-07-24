#ifndef RPIPLAYER_H_
#define RPIPLAYER_H_

#include "android/os/LooperThread.h"
#include "NetHandler.h"
#include "android/lang/String.h"
#include "android/util/List.h"
extern "C" {
#include "bcm_host.h"
#include "ilclient.h"
}

namespace android {
namespace util {
class Buffer;
}
}

using android::os::sp;

class RPiPlayer :
	public android::os::Handler
{
public:
	static const uint32_t NOTIFY_QUEUE_AUDIO_BUFFER = 1;
	static const uint32_t NOTIFY_QUEUE_VIDEO_BUFFER = 2;
	static const uint32_t NOTIFY_PLAY_AUDIO_BUFFER = 3;
	static const uint32_t NOTIFY_PLAY_VIDEO_BUFFER = 4;
	static const uint32_t STOP_MEDIA_SOURCE_DONE = 5;
	static const uint32_t NOTIFY_FILL_INPUT_BUFFERS = 6;
	static const uint32_t NOTIFY_INPUT_BUFFER_FILLED = 7;
	static const uint32_t NOTIFY_EMPTY_OMX_BUFFER = 8;

	RPiPlayer();
	virtual ~RPiPlayer();
	bool start(android::lang::String url);
	void stop();

	virtual void handleMessage(const sp<android::os::Message>& message);

private:
	bool startMediaSource(const android::lang::String& url);
	void stopMediaSource();
	int initOMXAudio();
	bool setAudioSink(const char* sinkName);
	void finalizeOMXAudio();
	int initOMXVideo();
	void finalizeOMXVideo();
	void onPlayAudioBuffer(const sp<android::util::Buffer>& accessUnit);
	void onFillInputBuffers();
	void onInputBufferFilled();
	void onPlayVideoBuffer(const sp<android::util::Buffer>& accessUnit);
	static void onEmptyBufferDone(void* args, COMPONENT_T* component);
	uint32_t getSamplesInOmx();
	void printLatencyConfig();
	void adjustTiming();


	sp< android::os::LooperThread<NetHandler> > mNetLooper;
	android::util::List< sp<android::util::Buffer> > mAudioAccessUnits;
	android::util::List< sp<android::util::Buffer> > mVideoAccessUnits;

	// Audio
	ILCLIENT_T* mAudioClient;
	COMPONENT_T* mAudioRenderer;
	COMPONENT_T* mAudioComponentList[2];
	OMX_BUFFERHEADERTYPE* mAudioBuffer;
	bool mFirstPacketAudio;

	// Video
	ILCLIENT_T* mVideoClient;
	TUNNEL_T mTunnel[4];
	COMPONENT_T* mVideoComponentList[5];
	COMPONENT_T* mVideoDecoder;
	COMPONENT_T* mVideoScheduler;
	COMPONENT_T* mVideoRenderer;
	COMPONENT_T* mClock;
	bool mPortSettingsChanged;
	bool mFirstPacketVideo;

	android::util::List< OMX_BUFFERHEADERTYPE* > mFilledOmxInputBuffers;
	android::util::List< OMX_BUFFERHEADERTYPE* > mEmptyOmxInputBuffers;
};

#endif /* RPIPLAYER_H_ */
