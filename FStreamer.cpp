// FStreamer.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include "stdafx.h"

#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"

#include "CFPlayer.h"
#include "fs_internal.h"
#include "player_queue.h"


extern "C"
{
#include "sdl/SDL.h"
#include "sdl/SDL_thread.h"

}

#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avdevice.lib")
#pragma comment(lib, "avfilter.lib")
#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "swresample.lib")
#pragma comment(lib, "swscale.lib")

#pragma comment(lib, "BasicUsageEnvironment.lib")
#pragma comment(lib, "groupsock.lib")
#pragma comment(lib, "liveMedia.lib")
#pragma comment(lib, "UsageEnvironment.lib")
#pragma comment(lib, "Ws2_32.lib")


#pragma comment(lib, "SDL.lib")
#pragma comment(lib, "SDLmain.lib")
#pragma comment(lib,"winmm.lib")
#pragma comment(lib,"dxguid.lib")

#pragma comment(linker,"/NODEFAULTLIB:msvcrtd.lib")

struct FStreamer
{
	 PlayerState player_state;
//	struct ourRTSPClient rtsp_client;

	SDL_Thread *rtsp_thread_id;
	SDL_Thread *player_thread_id;
};

extern void openURL(UsageEnvironment& env, char const* progName, char const* rtspURL ,PlayerState *ps);


char eventLoopWatchVariable = 0;
int rtsp_main(void *arg) {
  // Begin by setting up our usage environment:
  TaskScheduler* scheduler = BasicTaskScheduler::createNew();
  UsageEnvironment* env = BasicUsageEnvironment::createNew(*scheduler);

  //// We need at least one "rtsp://" URL argument:
  //if (argc < 2) {
  //  usage(*env, argv[0]);
  //  return 1;
  //}

  FStreamer *fs = (FStreamer *)arg;

  char* argv[3] ={"cf_player","rtsp://10.214.8.81:8554/h264ESVideoTest","rtsp://10.214.8.81:8554/mp3AudioTest"};
//   char* argv[2] ={"cf_player","rtsp://10.214.8.81:8554/mp3AudioTest"};
  // There are argc-1 URLs: argv[1] through argv[argc-1].  Open and start streaming each one:
  for (int i = 0; i < 1; ++i) {
    openURL(*env, argv[0], argv[1],&fs->player_state);
  }

  // All subsequent activity takes place within the event loop:
  env->taskScheduler().doEventLoop(&eventLoopWatchVariable);
    // This function call does not return, unless, at some point in time, "eventLoopWatchVariable" gets set to something non-zero.



  // If you choose to continue the application past this point (i.e., if you comment out the "return 0;" statement above),
  // and if you don't intend to do anything more with the "TaskScheduler" and "UsageEnvironment" objects,
  // then you can also reclaim the (small) memory used by these objects by uncommenting the following code:
  
    env->reclaim(); env = NULL;
    delete scheduler; scheduler = NULL;

	return 0;
  
}

int player_main( FStreamer *fs)
{
	 int flags;
	 PlayerState *ps = &fs->player_state;
    
    char dummy_videodriver[] = "SDL_VIDEODRIVER=dummy";

    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    
    // register all codecs, demux and protocols 
    avcodec_register_all();

	av_register_all();

	 flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
   
    if (SDL_Init (flags)) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        fprintf(stderr, "(Did you set the DISPLAY variable?)\n");
        exit(1);
    }

	SDL_EventState(SDL_ACTIVEEVENT, SDL_IGNORE);
    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
    SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

	

	struct AudioParams src;
	src.audio_codec_id = CODEC_ID_MP3;
	struct AudioParams tgt;
	tgt.channels = 2;
	tgt.channel_layout = av_get_default_channel_layout(tgt.channels);
	tgt.fmt = AV_SAMPLE_FMT_S16;
	tgt.freq = 44100;
	player_prepare_audio_stream(ps,src,tgt);

	ps->av_sync_type = AV_SYNC_AUDIO_MASTER;


	//struct VideoParams vparams;
	//vparams.tgt_fmt = PIX_FMT_YUV420P;
	//vparams.tgt_height = 0;
	//vparams.tgt_width = 0;
	//vparams.video_codec_id = AV_CODEC_ID_H264;

	//player_prepare_video(ps,vparams);

//	player_start_video(ps);




	player_start(ps);


	event_loop(ps);

	player_stop(ps);
	player_destroy(ps);

	return 0;
}



int _tmain(int argc, _TCHAR* argv[])
{
	FStreamer fs;


	memset(&fs,0,sizeof(FStreamer));

	fs.rtsp_thread_id =SDL_CreateThread(rtsp_main,&fs);

	player_main(&fs);

//	SDL_WaitThread(fs.rtsp_thread_id,NULL);
	
	return 0;
}

