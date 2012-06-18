#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <sys/resource.h>
#include "RtspSocket.h"
#include "android/lang/String.h"
#include "android/net/DatagramSocket.h"
extern "C" {
#include "bcm_host.h"
#include "ilclient.h"
}

using namespace android::os;
using namespace android::lang;
using namespace android::net;

static TUNNEL_T tunnel[4];
static COMPONENT_T* list[5];
static ILCLIENT_T* client;
static COMPONENT_T *video_decode = NULL, *video_scheduler = NULL, *video_render = NULL, *omx_clock = NULL;

static int initOMX() {
	OMX_VIDEO_PARAM_PORTFORMATTYPE format;
	OMX_TIME_CONFIG_CLOCKSTATETYPE cstate;
	unsigned char* rtpPacketData = NULL;

	printf("Initializing OMX...\n");

	bcm_host_init();

	memset(list, 0, sizeof(list));
	memset(tunnel, 0, sizeof(tunnel));

	if ((client = ilclient_init()) == NULL) {
		return -1;
	}

	if (OMX_Init() != OMX_ErrorNone) {
		ilclient_destroy(client);
		return -2;
	}

	// create video_decode
	if (ilclient_create_component(client, &video_decode, "video_decode", (ILCLIENT_CREATE_FLAGS_T)(ILCLIENT_DISABLE_ALL_PORTS | ILCLIENT_ENABLE_INPUT_BUFFERS)) != 0) {
		return -3;
	}
	list[0] = video_decode;

	// create video_render
	if (ilclient_create_component(client, &video_render, "video_render", ILCLIENT_DISABLE_ALL_PORTS) != 0) {
		return -4;
	}
	list[1] = video_render;

	// create clock
	if (ilclient_create_component(client, &omx_clock, "clock", ILCLIENT_DISABLE_ALL_PORTS) != 0) {
		return -5;
	}
	list[2] = omx_clock;

	memset(&cstate, 0, sizeof(cstate));
	cstate.nSize = sizeof(cstate);
	cstate.nVersion.nVersion = OMX_VERSION;
	cstate.eState = OMX_TIME_ClockStateWaitingForStartTime;
	cstate.nWaitMask = 1;
	if (OMX_SetParameter(ILC_GET_HANDLE(omx_clock), OMX_IndexConfigTimeClockState, &cstate) != OMX_ErrorNone) {
		return -6;
	}

	// create video_scheduler
	if (ilclient_create_component(client, &video_scheduler, "video_scheduler", ILCLIENT_DISABLE_ALL_PORTS) != 0) {
		return -7;
	}
	list[3] = video_scheduler;

	set_tunnel(tunnel, video_decode, 131, video_scheduler, 10);
	set_tunnel(tunnel+1, video_scheduler, 11, video_render, 90);
	set_tunnel(tunnel+2, omx_clock, 80, video_scheduler, 12);

	// setup clock tunnel first
	if (ilclient_setup_tunnel(tunnel+2, 0, 0) != 0) {
		return -8;
	} else {
		ilclient_change_component_state(omx_clock, OMX_StateExecuting);
	}

	ilclient_change_component_state(video_decode, OMX_StateIdle);

	memset(&format, 0, sizeof(OMX_VIDEO_PARAM_PORTFORMATTYPE));
	format.nSize = sizeof(OMX_VIDEO_PARAM_PORTFORMATTYPE);
	format.nVersion.nVersion = OMX_VERSION;
	format.nPortIndex = 130;
	format.eCompressionFormat = OMX_VIDEO_CodingAVC;

	if (OMX_SetParameter(ILC_GET_HANDLE(video_decode), OMX_IndexParamVideoPortFormat, &format) == OMX_ErrorNone &&
			ilclient_enable_port_buffers(video_decode, 130, NULL, NULL, NULL) == 0) {
	  ilclient_change_component_state(video_decode, OMX_StateExecuting);
	  printf("OMX init done.\n");
	  return 0;
	} else {
		printf("OMX init failed!\n");
		return -9;
	}
}

static void finalizeOMX() {
	ilclient_disable_tunnel(tunnel);
	ilclient_disable_tunnel(tunnel + 1);
	ilclient_disable_tunnel(tunnel + 2);
	ilclient_teardown_tunnels(tunnel);

	ilclient_state_transition(list, OMX_StateIdle);
	ilclient_state_transition(list, OMX_StateLoaded);

	ilclient_cleanup_components(list);

	OMX_Deinit();

	ilclient_destroy(client);
}

int main(int argc, char**argv)
{
	char* strIpAddress = NULL;
	char* strPort = "9000";

	if (argc < 2) {
		printf("Usage: <IP-Address> [<Port>]\n");
		return -1;
	}
	strIpAddress = argv[1];
	if (argc == 3) {
		strPort = argv[2];
	}
	uint16_t port = atoi(strPort);
	
	setpriority(PRIO_PROCESS, 0, -19);

	if (initOMX() != 0) {
		printf("OMX init failed!\n");
		return -1;
	}

	sp<RtspSocket> mSocket = new RtspSocket();
	if (!mSocket->connect(strIpAddress, port)) {
		printf("Cannot connect to server %s:%d\n", strIpAddress, port);
		return -1;
	}

	uint8_t rtspResponse[4096];

	String optionsMessage = String::format("OPTIONS rtsp://%s:%d/Test.sdp RTSP/1.0\r\nCSeq: 1\r\n\r\n", strIpAddress, strPort);
	mSocket->write(optionsMessage.c_str(), optionsMessage.size());
	int32_t size = mSocket->readPacket(rtspResponse, 4096);
	rtspResponse[size] = '\0';
	printf("OPTIONS: %s\n", rtspResponse);

	String describeMessage = String::format("DESCRIBE rtsp://%s:%d/Test.sdp RTSP/1.0\r\nCSeq: 2\r\n\r\n", strIpAddress, strPort);
	mSocket->write(describeMessage.c_str(), describeMessage.size());
	size = mSocket->readPacket(rtspResponse, 4096);
	rtspResponse[size] = '\0';
	printf("DESCRIBE: %s\n", rtspResponse);

	sp<DatagramSocket> rtpSocket = new DatagramSocket(56098);
	sp<DatagramSocket> rtcpSocket = new DatagramSocket(56099);

	String setupMessage = String::format("SETUP rtsp://%s:%d/Test.sdp RTSP/1.0\r\nCSeq: 3\r\nTransport: RTP/AVP;unicast;client_port=56098-56099\r\n\r\n", strIpAddress, strPort);
	mSocket->write(setupMessage.c_str(), setupMessage.size());
	size = mSocket->readPacket(rtspResponse, 4096);
	rtspResponse[size] = '\0';
	printf("SETUP: %s\n", rtspResponse);
	char* session = "Session: ";
	session = strstr((char*) rtspResponse, session);
	session += strlen("Session: ");
	int i = 0;
	while (*(session + i) != '\r') {
		i++;	
	}
	String sessionId(session, i);

	String playMessage = String::format("PLAY rtsp://%s:%d/Test.sdp RTSP/1.0\r\nCSeq: 4\r\nRange: npt=0.000-\r\nSession: %s\r\n\r\n", strIpAddress, strPort, sessionId.c_str());
	mSocket->write(playMessage.c_str(), playMessage.size());
	size = mSocket->readPacket(rtspResponse, 4096);
	rtspResponse[size] = '\0';
	printf("PLAY: %s\n", rtspResponse);


	uint8_t rtpPacketData[4096];
	int rtpPacketSize;

	printf("Waiting for H.264 rtpPacketData...\n");
	uint8_t nalHeader;
	bool startUnit = true;
	int port_settings_changed = 0;

	unsigned int data_len = 0;

	while (true) {
		rtpPacketSize = rtpSocket->recv(rtpPacketData, 4096);
		if (rtpPacketSize <= 0) {
			break;
		} else if (rtpPacketSize < 12) {
			continue;
		}
		if ((rtpPacketData[0] >> 6) != 2) { // RTPv2
			return -1;
		}
		if (rtpPacketData[0] & 0x20) { // padding
			size_t paddingSize = rtpPacketData[rtpPacketSize - 1];
			if (paddingSize + 12 > rtpPacketSize) {
				// If we removed this much padding we'd end up with something
				// that's too short to be a valid RTP header.
				continue;
			}
			rtpPacketSize -= paddingSize;
		}
		int numCSRCs = rtpPacketData[0] & 0x0F;
		size_t payloadOffset = 12 + 4 * numCSRCs;
		if (rtpPacketSize < payloadOffset) {
			// Not enough rtpPacketData to fit the basic header and all the CSRC entries.
			continue;
		}
		if (rtpPacketData[0] & 0x10) {
			// Header eXtension present.
			if (rtpPacketSize < payloadOffset + 4) {
				// Not enough rtpPacketData to fit the basic header, all CSRC entries
				// and the first 4 bytes of the extension header.
				continue;
			}

			const uint8_t* extensionData = &rtpPacketData[payloadOffset];
			size_t extensionLength = 4 * (extensionData[2] << 8 | extensionData[3]);
			if (rtpPacketSize < payloadOffset + 4 + extensionLength) {
				continue;
			}
			payloadOffset += 4 + extensionLength;
		}

		bool nalUnitBegins = false;
		uint8_t* h264Data = NULL;
		unsigned nalUnitType = rtpPacketData[payloadOffset] & 0x1F;
		bool printDT = false;
		int status = 0;

		if (nalUnitType >= 1 && nalUnitType <= 23) {
			h264Data = &rtpPacketData[payloadOffset];
			nalUnitBegins = true;
			uint32_t nri = (rtpPacketData[payloadOffset] >> 5) & 3;
			printf("NAL type: %d %d\n", nalUnitType, nri);
		} else if (nalUnitType == 28) { // FU-A
			h264Data = &rtpPacketData[payloadOffset + 2];
			if (startUnit || (rtpPacketData[payloadOffset + 1] & 0x80)) {
				uint32_t nalType = rtpPacketData[payloadOffset + 1] & 0x1f;
				uint32_t nri = (rtpPacketData[payloadOffset + 0] >> 5) & 3;
				nalHeader = (nri << 5) | nalType;
				nalUnitBegins = true;
				startUnit = false;
				printf("FU NAL type: %d %d\n", nalType, nri);
			}
			if (rtpPacketData[payloadOffset + 1] & 0x40) {
			}
		} else if (nalUnitType == 24) {
			printf("Oops\n");
			continue;
		} else {
			printf("Unknown NAL Type: %d\n", nalUnitType);
			continue;
		}

		OMX_BUFFERHEADERTYPE *buf;
		int first_packet = 1;

		if ((buf = ilclient_get_input_buffer(video_decode, 130, 1)) != NULL)
		{
			// feed rtpPacketData and wait until we get port settings changed
			//printf("Buffer: %x\n", buf);
			unsigned char *dest = buf->pBuffer;

			uint32_t offset = 0;
			if (nalUnitBegins) {
				memcpy(dest, "\x00\x00\x00\x01", 4);
				offset += 4;
				if (nalUnitType == 28) { // FU-A
					memcpy(dest + offset, &nalHeader, 1);
					offset += 1;
//					printf("FU NAL type: %d\n", nalHeader & 0x1f);
//					printf("Added NAL unit header: 0x%02X\n", nalHeader);
				}
			}
			uint32_t dataSize;
			if (nalUnitType >= 1 && nalUnitType <= 23) {
				dataSize = rtpPacketSize - payloadOffset;
			} else {
				dataSize = rtpPacketSize - payloadOffset - 2;
			}
			memcpy(dest + offset, h264Data, dataSize);
			data_len += offset + dataSize;

			if(port_settings_changed == 0  &&
			((data_len > 0 && ilclient_remove_event(video_decode, OMX_EventPortSettingsChanged, 131, 0, 0, 1) == 0) ||
			 (data_len == 0 && ilclient_wait_for_event(video_decode, OMX_EventPortSettingsChanged, 131, 0, 0, 1,
													   ILCLIENT_EVENT_ERROR | ILCLIENT_PARAMETER_CHANGED, 10000) == 0)))
			{
				port_settings_changed = 1;

				if(ilclient_setup_tunnel(tunnel, 0, 0) != 0)
				{
				   status = -7;
				   break;
				}

				ilclient_change_component_state(video_scheduler, OMX_StateExecuting);

				// now setup tunnel to video_render
				if(ilclient_setup_tunnel(tunnel+1, 0, 1000) != 0)
				{
				   status = -12;
				   break;
				}

				ilclient_change_component_state(video_render, OMX_StateExecuting);
			}
			if(!data_len)
				break;

			buf->nFilledLen = data_len;
			data_len = 0;

			buf->nOffset = 0;
			if(first_packet)
			{
				buf->nFlags = OMX_BUFFERFLAG_STARTTIME;
				first_packet = 0;
			}
			else
				buf->nFlags = OMX_BUFFERFLAG_TIME_UNKNOWN;

			if(OMX_EmptyThisBuffer(ILC_GET_HANDLE(video_decode), buf) != OMX_ErrorNone)
			{
				status = -6;
				break;
			}
		}

//		printf("Done\n");
//
//		buf->nFilledLen = 0;
//		buf->nFlags = OMX_BUFFERFLAG_TIME_UNKNOWN | OMX_BUFFERFLAG_EOS;
//
//		if(OMX_EmptyThisBuffer(ILC_GET_HANDLE(video_decode), buf) != OMX_ErrorNone)
//		status = -20;
//
//		// wait for EOS from render
//		ilclient_wait_for_event(video_render, OMX_EventBufferFlag, 90, 0, OMX_BUFFERFLAG_EOS, 0,
//						  ILCLIENT_BUFFER_FLAG_EOS, 10000);
//
//		// need to flush the renderer to allow video_decode to disable its input port
//		ilclient_flush_tunnels(tunnel, 0);
//
//		ilclient_disable_port_buffers(video_decode, 130, NULL, NULL, NULL);
//
//		printf("%d\n", rtpPacketSize);
	}

	finalizeOMX();

	return 0;
}
