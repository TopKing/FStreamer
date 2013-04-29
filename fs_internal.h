
#ifndef _FS_INTERNAL_H
#define _FS_INTERNAL_H

extern "C"
{
	#include "libavutil/avstring.h"
	#include "libavutil/colorspace.h"
	#include "libavutil/mathematics.h"
	#include "libavutil/pixdesc.h"
	#include "libavutil/imgutils.h"
	#include "libavutil/dict.h"
	#include "libavutil/parseutils.h"
	#include "libavutil/samplefmt.h"
	#include "libavutil/avassert.h"
	#include "libavutil/time.h"
	#include "libavformat/avformat.h"
	#include "libavdevice/avdevice.h"
	#include "libswscale/swscale.h"
	#include "libavutil/opt.h"
	#include "libavcodec/avfft.h"
	#include "libswresample/swresample.h"
	#include "sdl/SDL.h"
	#include "sdl/SDL_thread.h"
}
#include "player_queue.h"


typedef struct AudioParams {
    int freq;
    int channels;
    int channel_layout;
    enum AVSampleFormat fmt;
	enum AVCodecID audio_codec_id;
} AudioParams;

typedef struct VideoParams {
    enum PixelFormat tgt_fmt;
	int tgt_width;
	int tgt_height;
	enum AVCodecID video_codec_id;
} VideoParams;

typedef struct VideoPicture {
    double pts;                                  ///< presentation time stamp for this picture
    int64_t pos;                                 ///< byte position in file
    int skip;
    SDL_Overlay *bmp;
    int width, height; /* source height & width */
    AVRational sample_aspect_ratio;
	int allocated;
	int reallocate;

} VideoPicture;

enum {
    AV_SYNC_AUDIO_MASTER, /* default choice */
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
};

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_QUEUE_SIZE 4

#define VIDEO_PICTURE_QUEUE_SIZE 4
typedef struct PlayerState {

     SDL_Thread *read_tid;
	 int request_abort;
	 SDL_cond *continue_read_thread;
	 SDL_mutex *read_wait_mutex;

	//******************audio******************
	 PacketQueue audioq;
	 struct AudioParams audio_params_src;
	 struct AudioParams audio_params_tgt;
	 AVCodecContext *audio_ctx;

	 uint8_t audio_silence_buf[SDL_AUDIO_BUFFER_SIZE];  //没有解码输出时，静音

	 //保存解码后音频
     uint8_t *audio_buf;
     unsigned int audio_buf_size; //* in bytes 
	 int audio_buf_index; //* in bytes //解码后数据读取位置
	 int audio_hw_buf_size;
	 int audio_write_buf_size;

	 uint8_t *audio_buf1;

	 DECLARE_ALIGNED(16,uint8_t,audio_buf2)[AVCODEC_MAX_AUDIO_FRAME_SIZE * 4];  //用于解码后重采样

	 AVFrame *audio_frame; //for decode 

	 //解码时一帧可能有多个帧，下面用来保存正在解码的包
	 AVPacket audio_pkt_temp;         //保存解码包的解码状态
     AVPacket audio_pkt;              //保存原始解码包

	 SwrContext *audio_swr_ctx;      //解码后，进行重采样 

	 AVFormatContext *audio_fmtctx;


	 //**************************video**************************
	 SDL_Thread* video_tid;
	 SDL_Thread* refresh_tid;
	 PacketQueue videoq;
	 VideoPicture pictq[VIDEO_PICTURE_QUEUE_SIZE];
	 int pictq_rindex;
	 int pictq_windex;
	 int pictq_size;
	 SDL_mutex *pictq_mutex;
	 SDL_cond *pictq_cond;
	 AVCodecContext *video_ctx;
	 struct SwsContext *img_convert_ctx;
	 struct VideoParams video_prams;
	 int refresh;
	

	 int width;
	 int height;
	 
	 int has_audio;
	 int has_video;
	 int isPaused;
	 int force_refresh;


	 ///************************syn*****************
	 int av_sync_type ;
     double external_clock; /* external clock base */
     int64_t external_clock_time;

     double audio_clock;
	 double audio_current_pts;
     double audio_current_pts_drift;
     int frame_drops_early;
     int frame_drops_late;

	 double frame_timer;
     double frame_last_pts;
     double frame_last_duration;
     double frame_last_dropped_pts;
     double frame_last_returned_time;
     double frame_last_filter_delay;
     int64_t frame_last_dropped_pos;
     double video_clock;                          ///< pts of last decoded frame / predicted pts of next decoded frame

	 double video_current_pts;                    ///< current displayed pts (different from video_clock if frame fifos are used)
     double video_current_pts_drift;              ///< video_current_pts - time (av_gettime) at which we updated video_current_pts - used to have running video pts
     int64_t video_current_pos;                   ///< current displayed file pos
	 

	 SDL_Surface *screen;
     int is_full_screen ;
	 int fs_screen_width;
	 int fs_screen_height ;
	 int screen_width ;
	 int screen_height ;
	 char* window_title;

	 //PlayerState()
	 //{
		// audio_fmtctx = NULL;
		// audio_frame = NULL;
		// audio_swr_ctx = NULL;

		// pictq_mutex = NULL;
		// pictq_cond = NULL;
		// video_ctx = NULL;
	 //}
} PlayerState;


#endif