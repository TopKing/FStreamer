
#include "config.h"

#include "player_log.h"

#include "stdafx.h"
#include "player_queue.h"
#include "fs_internal.h"
#include "CFPlayer.h"


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

#include <Windows.h>



#define DEBUG 1



#define FF_ALLOC_EVENT   (SDL_USEREVENT)
#define FF_REFRESH_EVENT (SDL_USEREVENT + 1)
#define FF_QUIT_EVENT    (SDL_USEREVENT + 2)

//*****************global variable*********************


/* no AV sync correction is done if below the AV sync threshold */
#define AV_SYNC_THRESHOLD 0.01
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 30.0

static int av_sync_type = AV_SYNC_AUDIO_MASTER;
static int64_t start_time = AV_NOPTS_VALUE;
static int64_t duration = AV_NOPTS_VALUE;
static int decoder_reorder_pts = -1;
static int framedrop = -1;


static int sws_flags = SWS_FAST_BILINEAR;

 //int player_start(PlayerState *player);
 //int player_start_audio(PlayerState *player);
 //int player_prepare_video(PlayerState *player,VideoParams &vprams);
 //int player_start_video(PlayerState *player);
static int do_exit(PlayerState *ps);

PlayerState * player_create()
{
	PlayerState * player = (PlayerState *)malloc(sizeof(PlayerState));
	player->read_tid = NULL;

	memset(&player->audio_params_src,0,sizeof(AudioParams));
	memset(&player->audio_params_tgt,0,sizeof(AudioParams));

	player->audio_buf = NULL;
	player->audio_buf_size = 0;
	player->audio_buf_index = 0;

	player->audio_ctx = NULL;
	player->audio_frame = NULL;

	player->request_abort = 1;

	return player;
}

 int player_stop(PlayerState *player)
{
	SDL_PauseAudio(1);
	player->request_abort = 1;
//	SDL_CondSignal(player->continue_read_thread);
	packet_queue_abort(&player->audioq);
	return 0;
}

 int player_destroy(PlayerState *player)
{
	SDL_Quit();
	//if(player->continue_read_thread)
	//{
	//	SDL_DestroyCond(player->continue_read_thread);
	//	player->continue_read_thread = NULL;
	//}

	//if(player->read_wait_mutex)
	//{
	//	SDL_DestroyMutex(player->read_wait_mutex);
	//	player->read_wait_mutex = NULL;
	//}

	if(player->audio_ctx)
	{
		avcodec_close(player->audio_ctx);
		av_free(player->audio_ctx);
		player->audio_ctx = NULL;
	}

	return 0;
}


 int player_prepare_audio_stream(PlayerState *player,struct AudioParams &src , struct AudioParams &tgt)
{
	AVCodec *audio_codec = NULL;
	if(player == NULL)
		return -1;

	player->audio_params_src = src;
	player->audio_params_tgt = tgt;

	audio_codec = avcodec_find_decoder(src.audio_codec_id);
	
	if(!audio_codec)
	{
		LOG("can not find audio decoder!\n");
		goto fail;
	}

	player->audio_ctx = avcodec_alloc_context3(audio_codec);
	if(!player->audio_ctx)
	{
		LOG("can not alloc audio codec context!\n");
		goto fail;
	}
	//player->audio_ctx = player->audio_fmtctx->streams[0]->codec;
	//audio_codec = avcodec_find_decoder(player->audio_ctx->codec_id);

	if (audio_codec->capabilities & CODEC_CAP_TRUNCATED)  
    {  
        player->audio_ctx->flags |= CODEC_CAP_TRUNCATED;  
    }  

	player->audio_ctx->sample_fmt = AV_SAMPLE_FMT_S16;
//	player->audio_ctx->bit_rate  = 12200;
	player->audio_ctx->codec_type = AVMEDIA_TYPE_AUDIO;
	player->audio_ctx->sample_rate = 48000;
	player->audio_ctx->channels =2;
	

	if(avcodec_open2(player->audio_ctx, audio_codec, NULL) < 0)
	{
		LOG("can not open audio codec !\n");
		goto fail;
	}

	packet_queue_init(&player->audioq);

	return 0;

fail:
	if(!player->audio_ctx)
	{
		avcodec_close(player->audio_ctx);
		player->audio_ctx = NULL;
	}
	av_free(player->audio_ctx);
	return -1;
}

/* get the current audio clock value */
static double get_audio_clock(PlayerState *ps)
{
    if (ps->isPaused) {
        return ps->audio_current_pts;
    } else {
        return ps->audio_current_pts_drift + av_gettime() / 1000000.0;
    }
}

/* get the current video clock value */
static double get_video_clock(PlayerState *ps)
{
    if (ps->isPaused) {
        return ps->video_current_pts;
    } else {
        return ps->video_current_pts_drift + av_gettime() / 1000000.0;
    }
}

/* get the current external clock value */
static double get_external_clock(PlayerState *ps)
{
    int64_t ti;
    ti = av_gettime();
    return ps->external_clock + ((ti - ps->external_clock_time) * 1e-6);
}

/* get the current master clock value */
static double get_master_clock(PlayerState *ps)
{
    double val;

    if (ps->av_sync_type == AV_SYNC_VIDEO_MASTER) {
        if (ps->has_video)
            val = get_video_clock(ps);
        else
            val = get_audio_clock(ps);
    } else if (ps->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        if (ps->has_audio)
            val = get_audio_clock(ps);
        else
            val = get_video_clock(ps);
    } else {
        val = get_external_clock(ps);
    }
    return val;
}


static int read_thread(void *args)
{
	PlayerState *ps = (PlayerState *)args;

	AVFormatContext *avfmtctx = NULL;	

	int ret = -1;

	char *filename = "I:\\vc\\ffplay\\Debug\\blood.mp4";
	
	ret = avformat_open_input(&avfmtctx,filename,NULL,NULL);
	if(ret < 0)
	{
		LOG("open file failed!\n");
		return ret;
	}

	av_dump_format(avfmtctx,0,filename,false);

	ret = avformat_find_stream_info(avfmtctx,NULL)  ;
    if(ret < 0)
	{
		return ret;
	}

	int audiostream_index = -1;
	 for(int j=0; j<avfmtctx->nb_streams; j++)//找到音频对应的stream
	 {
        if(avfmtctx->streams[j]->codec->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            audiostream_index=j;
            break;
        }
	 }

	 AVCodecContext *codectx = avfmtctx->streams[audiostream_index]->codec;

	 int videostream_index = -1;
	 for(int j=0; j<avfmtctx->nb_streams; j++)//找到音频对应的stream
	 {
        if(avfmtctx->streams[j]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            videostream_index=j;
            break;
        }
	 }

    if(audiostream_index == -1 && videostream_index == -1)
    {
        LOG("input file has no audio stream\n");
        return -1; // Didn't find a audio stream
    }

	ps->audio_fmtctx = avfmtctx;




	struct VideoParams vparams;
	vparams.tgt_fmt = PIX_FMT_YUV420P;
	vparams.tgt_height = 512;
	vparams.tgt_width = 512;
	vparams.video_codec_id = AV_CODEC_ID_H264;

//	player_prepare_video(ps,vparams);

	player_start_video(ps);


	int eof = 0;
	AVPacket pkt1,*pkt=&pkt1;

	av_init_packet(pkt);

	while(1)
	{
		if(ps->request_abort)
		{
			break;
		}
		
//		if(ps->audioq.nb_packets == MAX_AUDIO_QUEUE_SIZE)
//		{
//			SDL_LockMutex(ps->read_wait_mutex);
//			SDL_CondWaitTimeout(ps->continue_read_thread,ps->read_wait_mutex,10);
//
////			SDL_CondWait(ps->continue_read_thread, ps->read_wait_mutex);
//
//			SDL_UnlockMutex(ps->read_wait_mutex);
//			continue;
//		}

		ret = av_read_frame(avfmtctx,pkt);
		if( ret < 0)
		{
			if (ret == AVERROR_EOF || url_feof(avfmtctx->pb))
                eof = 1;
            if (avfmtctx->pb && avfmtctx->pb->error)
                break;
            //SDL_LockMutex(ps->read_wait_mutex);
            //SDL_CondWaitTimeout(ps->continue_read_thread, ps->read_wait_mutex, 10);
            //SDL_UnlockMutex(ps->read_wait_mutex);
            continue;
			
		}


		if(pkt->stream_index == audiostream_index)
		{
			if(pkt->size > 0 )
			{
			//if (!ps->audio_frame) {
			//		if (!(ps->audio_frame = avcodec_alloc_frame()))
			//			return AVERROR(ENOMEM);
			//	} else
			//		avcodec_get_frame_defaults(ps->audio_frame);

			//   int got_frame;
			//   copyPaket(ps,pkt);

			//	int len1 = avcodec_decode_audio4(ps->audio_ctx, ps->audio_frame, &got_frame, &ps->audio_pkt);

			//	LOG("got_frame = %d, len = %d\n",got_frame, len1);
				packet_queue_put(&ps->audioq,pkt);
				av_free_packet(pkt);
			}
		}

		if(pkt->stream_index == videostream_index)
		{
			if(pkt->size > 0)
			{
				packet_queue_put(&ps->videoq,pkt);
			}

			av_free_packet(pkt);

		}
		
		
	}

	av_close_input_file(avfmtctx);

	return 0 ;
}


//* decode one audio frame and returns its uncompressed size 
static int audio_decode_frame(PlayerState *ps, double *pts_ptr)
{
    AVPacket *pkt_temp = &ps->audio_pkt_temp;
    AVPacket *pkt = &ps->audio_pkt;
    AVCodecContext *dec = ps->audio_ctx;

    int len1, len2, data_size, resampled_data_size;
    int64_t dec_channel_layout;
    int got_frame;
    double pts;
    int new_packet = 0;
    int wanted_nb_samples;

    for (;;) {
        // NOTE: the audio packet can contain several frames 
        while (pkt_temp->size > 0 || (!pkt_temp->data && new_packet)) {
            if (!ps->audio_frame) {
                if (!(ps->audio_frame = avcodec_alloc_frame()))
                    return AVERROR(ENOMEM);
            } else
                avcodec_get_frame_defaults(ps->audio_frame);

            new_packet = 0;

            len1 = avcodec_decode_audio4(dec, ps->audio_frame, &got_frame, pkt_temp);
            if (len1 < 0) {
                // if error, we skip the frame 
                pkt_temp->size = 0;
                break;
            }

            pkt_temp->data += len1;
            pkt_temp->size -= len1;

            if (!got_frame) {
                // stop sending empty packets if the decoder is finished /
                if (!pkt_temp->data && dec->codec->capabilities & CODEC_CAP_DELAY)
                    break;

                continue;
            }
            data_size = av_samples_get_buffer_size(NULL, dec->channels,
                                                   ps->audio_frame->nb_samples,
                                                   dec->sample_fmt, 1);

            dec_channel_layout =
                (dec->channel_layout && dec->channels == av_get_channel_layout_nb_channels(dec->channel_layout)) ?
                dec->channel_layout : av_get_default_channel_layout(dec->channels);

			wanted_nb_samples = ps->audio_frame->nb_samples;

            if (dec->sample_fmt    != ps->audio_params_tgt.fmt            ||
                dec_channel_layout != ps->audio_params_tgt.channel_layout ||
                dec->sample_rate   != ps->audio_params_tgt.freq           ||
                (wanted_nb_samples != ps->audio_frame->nb_samples && !ps->audio_swr_ctx)) {
                swr_free(&ps->audio_swr_ctx);
                ps->audio_swr_ctx = swr_alloc_set_opts(NULL,
                                                 ps->audio_params_tgt.channel_layout, ps->audio_params_tgt.fmt, ps->audio_params_tgt.freq,
                                                 dec_channel_layout,           dec->sample_fmt,   dec->sample_rate,
                                                 0, NULL);
                if (!ps->audio_swr_ctx || swr_init(ps->audio_swr_ctx) < 0) {
                    fprintf(stderr, "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                        dec->sample_rate,   av_get_sample_fmt_name(dec->sample_fmt),   dec->channels,
                        ps->audio_params_tgt.freq, av_get_sample_fmt_name(ps->audio_params_tgt.fmt), ps->audio_params_tgt.channels);
                    break;
                }

                ps->audio_params_src.channel_layout = dec_channel_layout;
                ps->audio_params_src.channels = dec->channels;
                ps->audio_params_src.freq = dec->sample_rate;
                ps->audio_params_src.fmt = dec->sample_fmt;
            }

            if (ps->audio_swr_ctx) {
                const uint8_t **in = (const uint8_t **)ps->audio_frame->extended_data;
                uint8_t *out[] = {ps->audio_buf2};
                int out_count = sizeof(ps->audio_buf2) / ps->audio_params_tgt.channels / av_get_bytes_per_sample(ps->audio_params_tgt.fmt);

                if (wanted_nb_samples != ps->audio_frame->nb_samples) {

                    if (swr_set_compensation(ps->audio_swr_ctx, (wanted_nb_samples - ps->audio_frame->nb_samples) * ps->audio_params_tgt.freq / dec->sample_rate,
                                                wanted_nb_samples * ps->audio_params_tgt.freq / dec->sample_rate) < 0) {
                        fprintf(stderr, "swr_set_compensation() failed\n");
                        break;
                    }
                }
                len2 = swr_convert(ps->audio_swr_ctx, out, out_count, in, ps->audio_frame->nb_samples);
                if (len2 < 0) {
                    fprintf(stderr, "swr_convert() failed\n");
                    break;
                }
                if (len2 == out_count) {
                    fprintf(stderr, "warning: audio buffer is probably too small\n");
                    swr_init(ps->audio_swr_ctx);
                }
                ps->audio_buf = ps->audio_buf2;
                resampled_data_size = len2 * ps->audio_params_tgt.channels * av_get_bytes_per_sample(ps->audio_params_tgt.fmt);
            } else {
                ps->audio_buf = ps->audio_frame->data[0];
                resampled_data_size = data_size;
            }

			 /* if no pts, then compute it */
            pts = ps->audio_clock;
            *pts_ptr = pts;
            ps->audio_clock += (double)data_size /
                (dec->channels * dec->sample_rate * av_get_bytes_per_sample(dec->sample_fmt));
#ifdef DEBUG
            {
                static double last_clock;
                printf("audio: delay=%0.3f clock=%0.3f pts=%0.3f\n",
                       ps->audio_clock - last_clock,
                       ps->audio_clock, pts);
                last_clock = ps->audio_clock;
            }
#endif
          
            return resampled_data_size;
        }

        //* free the current packet 
        if (pkt->data)
		{
            av_free_packet(pkt);
			av_init_packet(pkt);
		}

        memset(pkt_temp, 0, sizeof(*pkt_temp));

        if (ps->audioq.abort_request) {
            return -1;
        }


		  //if (ps->audioq.nb_packets == 0)
    //        SDL_CondSignal(ps->continue_read_thread);

        //* read next packet 
        if ((new_packet = packet_queue_get(&ps->audioq, pkt, 1)) < 0)
		{
			pkt_temp->size = 0;
            return -1;
		}
		
        *pkt_temp = *pkt;

 /* if update the audio clock with the pts */
        if (pkt->pts != AV_NOPTS_VALUE) {
            ps->audio_clock = av_q2d(ps->audio_ctx->time_base)*pkt->pts;
        }

//		printf("audio pts=%I64u,audio_clock=%f \n",pkt->pts,ps->audio_clock);
    }
}


//* prepare a new audio buffer 
static void sdl_audio_callback(void *opaque, Uint8 *stream, int len)
{
    PlayerState *ps = (PlayerState *) opaque;
    int audio_size, len1;
    int bytes_per_sec;
    int frame_size = av_samples_get_buffer_size(NULL, ps->audio_params_tgt.channels, 1, ps->audio_params_tgt.fmt, 1);
    double pts;

	int64_t audio_callback_time = av_gettime();

    while (len > 0) {
        if (ps->audio_buf_index >= ps->audio_buf_size) {

           audio_size = audio_decode_frame(ps, &pts);
           if (audio_size < 0) {
                // if error, just output silence 
               ps->audio_buf      = ps->audio_silence_buf;
               ps->audio_buf_size = sizeof(ps->audio_silence_buf) / frame_size * frame_size;
           } else {
               
               ps->audio_buf_size = audio_size;
           }
           ps->audio_buf_index = 0;
        }

        len1 = ps->audio_buf_size - ps->audio_buf_index;

        if (len1 > len)
            len1 = len;
        memcpy(stream, (uint8_t *)ps->audio_buf + ps->audio_buf_index, len1);
        len -= len1;
        stream += len1;
        ps->audio_buf_index += len1;
    }
     bytes_per_sec = ps->audio_params_tgt.freq * ps->audio_params_tgt.channels * av_get_bytes_per_sample(ps->audio_params_tgt.fmt);
     ps->audio_write_buf_size = ps->audio_buf_size - ps->audio_buf_index;
	 ps->audio_current_pts = ps->audio_clock - (double)(2 * ps->audio_hw_buf_size + ps->audio_write_buf_size) / bytes_per_sec;
     ps->audio_current_pts_drift = ps->audio_current_pts - audio_callback_time / 1000000.0;

//	 printf("audio_current_pts pts=%f,audio_clock=%f \n",ps->audio_current_pts,ps->audio_clock);
   
}

static int sdl_audio_open(void *opaque, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams *audio_hw_params)
{
    SDL_AudioSpec wanted_spec, spec;
    const char *env;
    const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};

    env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {
        wanted_nb_channels = atoi(env);
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
    }
    if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
        wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
    }

    wanted_spec.channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
    wanted_spec.freq = wanted_sample_rate;

    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        fprintf(stderr, "Invalid sample rate or channel count!\n");
        return -1;
    }
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = opaque;
    while (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
        fprintf(stderr, "SDL_OpenAudio (%d channels): %s\n", wanted_spec.channels, SDL_GetError());
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!wanted_spec.channels) {
            fprintf(stderr, "No more channel combinations to try, audio open failed\n");
            return -1;
        }
        wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
    }
    if (spec.format != AUDIO_S16SYS) {
        fprintf(stderr, "SDL advised audio format %d is not supported!\n", spec.format);
        return -1;
    }
    if (spec.channels != wanted_spec.channels) {
        wanted_channel_layout = av_get_default_channel_layout(spec.channels);
        if (!wanted_channel_layout) {
            fprintf(stderr, "SDL advised channel count %d is not supported!\n", spec.channels);
            return -1;
        }
    }

    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;
    audio_hw_params->channel_layout = wanted_channel_layout;
    audio_hw_params->channels =  spec.channels;
    return spec.size;
}


//******************sdl video******************************
inline long rint(double x)
{
	if(x >= 0.)
		return (long)(x + 0.5);
	else
		return (long)(x - 0.5);
}

static inline void fill_rectangle(SDL_Surface *screen,
                                  int x, int y, int w, int h, int color)
{
    SDL_Rect rect;
    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;
    SDL_FillRect(screen, &rect, color);
}

static void calculate_display_rect(SDL_Rect *rect, int scr_xleft, int scr_ytop, int scr_width, int scr_height, VideoPicture *vp)
{
    float aspect_ratio;
    int width, height, x, y;

    if (vp->sample_aspect_ratio.num == 0)
        aspect_ratio = 0;
    else
        aspect_ratio = av_q2d(vp->sample_aspect_ratio);

    if (aspect_ratio <= 0.0)
        aspect_ratio = 1.0;
    aspect_ratio *= (float)vp->width / (float)vp->height;

    /* XXX: we suppose the screen has a 1.0 pixel ratio */
    height = scr_height;
    width = ((int)rint(height * aspect_ratio)) & ~1;
    if (width > scr_width) {
        width = scr_width;
        height = ((int)rint(width / aspect_ratio)) & ~1;
    }
    x = (scr_width - width) / 2;
    y = (scr_height - height) / 2;
    rect->x = scr_xleft + x;
    rect->y = scr_ytop  + y;
    rect->w = FFMAX(width,  1);
    rect->h = FFMAX(height, 1);
}



static void video_image_display(PlayerState *ps)
{
    VideoPicture *vp;
    AVPicture pict;
    SDL_Rect rect;
    int i;

    vp = &ps->pictq[ps->pictq_rindex];

    if (vp->bmp) {
        calculate_display_rect(&rect, 0, 0, ps->width, ps->height, vp);

        SDL_DisplayYUVOverlay(vp->bmp, &rect);
    }
}

static int video_open(PlayerState *ps, int force_set_video_mode)
{
    int flags = SDL_HWSURFACE | SDL_ASYNCBLIT | SDL_HWACCEL;
    int w,h;
    VideoPicture *vp = &ps->pictq[ps->pictq_rindex];
    SDL_Rect rect;

    if (ps->is_full_screen) flags |= SDL_FULLSCREEN;
    else                flags |= SDL_RESIZABLE;

    if (ps->is_full_screen && ps->fs_screen_width) {
        w = ps->fs_screen_width;
        h = ps->fs_screen_height;
    } else if (!ps->is_full_screen && ps->screen_width) {
        w = ps->screen_width;
        h = ps->screen_height;
    } else if (vp->width) {
        calculate_display_rect(&rect, 0, 0, INT_MAX, vp->height, vp);
        w = rect.w;
        h = rect.h;
    } else {
        w = 640;
        h = 480;
    }
    if (ps->screen && ps->width == ps->screen->w && ps->screen->w == w
       && ps->height== ps->screen->h && ps->screen->h == h && !force_set_video_mode)
        return 0;
    ps->screen = SDL_SetVideoMode(w, h, 0, flags);
    if (!ps->screen) {
        fprintf(stderr, "SDL: could not set video mode - exiting\n");
        do_exit(ps);
    }
    if (!ps->window_title)
        ps->window_title = "cf_play";
    SDL_WM_SetCaption(ps->window_title, ps->window_title);

    ps->width  = ps->screen->w ;
    ps->height = ps->screen->h ;

    return 0;
}

static void pictq_next_picture(PlayerState *ps) {
    /* update queue size and signal for next picture */
    if (++ps->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE)
        ps->pictq_rindex = 0;

    SDL_LockMutex(ps->pictq_mutex);
    ps->pictq_size--;
    SDL_CondSignal(ps->pictq_cond);
    SDL_UnlockMutex(ps->pictq_mutex);
}

static void pictq_prev_picture(PlayerState *ps) {
    VideoPicture *prevvp;
    /* update queue size and signal for the previous picture */
    prevvp = &ps->pictq[(ps->pictq_rindex + VIDEO_PICTURE_QUEUE_SIZE - 1) % VIDEO_PICTURE_QUEUE_SIZE];
    if (prevvp->allocated && !prevvp->skip) {
        SDL_LockMutex(ps->pictq_mutex);
        if (ps->pictq_size < VIDEO_PICTURE_QUEUE_SIZE - 1) {
            if (--ps->pictq_rindex == -1)
                ps->pictq_rindex = VIDEO_PICTURE_QUEUE_SIZE - 1;
            ps->pictq_size++;
        }
        SDL_CondSignal(ps->pictq_cond);
        SDL_UnlockMutex(ps->pictq_mutex);
    }
}

static double compute_target_delay(double delay, PlayerState *is)
{
    double sync_threshold, diff;

    /* update delay to follow master synchronisation source */
    if (((is->av_sync_type == AV_SYNC_AUDIO_MASTER && is->has_audio) ||
         is->av_sync_type == AV_SYNC_EXTERNAL_CLOCK)) {
        /* if video is slave, we try to correct big delays by
           duplicating or deleting a frame */
        diff = get_video_clock(is) - get_master_clock(is);

        /* skip or repeat frame. We take into account the
           delay to compute the threshold. I still don't know
           if it is the best guess */
        sync_threshold = FFMAX(AV_SYNC_THRESHOLD, delay);
        if (fabs(diff) < AV_NOSYNC_THRESHOLD) {
            if (diff <= -sync_threshold)
                delay = 0;
            else if (diff >= sync_threshold)
                delay = 2 * delay;
        }
		else if(diff < 0)
		{
			delay = 0;
		}
    }

    av_dlog(NULL, "video: delay=%0.3f A-V=%f\n",
            delay, -diff);

    return delay;
}


static void update_video_pts(PlayerState *is, double pts, int64_t pos) 
{
    double time = av_gettime() / 1000000.0;
    /* update current video pts */
    is->video_current_pts = pts;
    is->video_current_pts_drift = is->video_current_pts - time;
    is->video_current_pos = pos;
    is->frame_last_pts = pts;
}


/* display the current picture, if any */
static void video_display(PlayerState *is)
{
    if (!is->screen)
        video_open(is, 0);
  
    else if (is->has_video)
        video_image_display(is);
}

/* called to display each frame */
static void video_refresh(void *opaque)
{
    PlayerState *ps = (PlayerState*) opaque;
    VideoPicture *vp;
    double time;

    if (ps->has_video) {

        if (ps->force_refresh)
            pictq_prev_picture(ps);

retry:
        if (ps->pictq_size <= 0) 
		{
            SDL_LockMutex(ps->pictq_mutex);
            if (ps->frame_last_dropped_pts != AV_NOPTS_VALUE && ps->frame_last_dropped_pts > ps->frame_last_pts) {
                update_video_pts(ps, ps->frame_last_dropped_pts, ps->frame_last_dropped_pos);
                ps->frame_last_dropped_pts = AV_NOPTS_VALUE;
            }
            SDL_UnlockMutex(ps->pictq_mutex);

            // nothing to do, no picture to display in the que
			return;
        } 
		else 
		{
				double last_duration, duration, delay;
				/* dequeue the picture */
				vp = &ps->pictq[ps->pictq_rindex];

				if (vp->skip)
				{
					pictq_next_picture(ps);
					goto retry;
				}

				if (ps->isPaused)
					goto display;

				 /* compute nominal last_duration */
            last_duration = vp->pts - ps->frame_last_pts;
            if (last_duration > 0 && last_duration < 10.0) {
                /* if duration of the last frame was sane, update last_duration in video state */
                ps->frame_last_duration = last_duration;
            }
            delay = compute_target_delay(ps->frame_last_duration, ps);

            time= av_gettime()/1000000.0;
            if (time < ps->frame_timer + delay)
                return;

            if (delay > 0)
                ps->frame_timer += delay * FFMAX(1, floor((time-ps->frame_timer) / delay));

            SDL_LockMutex(ps->pictq_mutex);
            update_video_pts(ps, vp->pts, vp->pos);
            SDL_UnlockMutex(ps->pictq_mutex);

          
            if (ps->pictq_size > 1) {
                VideoPicture *nextvp = &ps->pictq[(ps->pictq_rindex + 1) % VIDEO_PICTURE_QUEUE_SIZE];
                duration = nextvp->pts - vp->pts;
                if((framedrop>0 || (framedrop && ps->has_audio)) && time > ps->frame_timer + duration){
                    ps->frame_drops_late++;
                    pictq_next_picture(ps);
                    goto retry;
                }
			}
           }

display:
          
            video_display(ps);

            pictq_next_picture(ps);
        
    } 
    ps->force_refresh = 0;

    
}

/* allocate a picture (needs to do that in main thread to avoid
   potential locking problems */
static void alloc_picture(PlayerState *is)
{
    VideoPicture *vp;

    vp = &is->pictq[is->pictq_windex];

    if (vp->bmp)
        SDL_FreeYUVOverlay(vp->bmp);


    video_open(is, 0);

    vp->bmp = SDL_CreateYUVOverlay(vp->width, vp->height,
                                   SDL_YV12_OVERLAY,
                                   is->screen);
    if (!vp->bmp || vp->bmp->pitches[0] < vp->width) {
        /* SDL allocates a buffer smaller than requested if the video
         * overlay hardware is unable to support the requested size. */
        fprintf(stderr, "Error: the video system does not support an image\n"
                        "size of %dx%d pixels. Try using -lowres or -vf \"scale=w:h\"\n"
                        "to reduce the image size.\n", vp->width, vp->height );
        do_exit(is);
    }

    SDL_LockMutex(is->pictq_mutex);
    vp->allocated = 1;  
    SDL_CondSignal(is->pictq_cond);
    SDL_UnlockMutex(is->pictq_mutex);
}

static int queue_picture(PlayerState *ps, AVFrame *src_frame, double pts1, int64_t pos)
{
    VideoPicture *vp;
    double frame_delay, pts = pts1;

	/* compute the exact PTS for the picture if it is omitted in the stream
     * pts1 is the dts of the pkt / pts of the frame */
    if (pts != 0) {
        /* update video clock with pts, if present */
        ps->video_clock = pts;
    } else {
        pts = ps->video_clock;
    }

//	printf("video decode pkt pts=%I64d, dts=%I64d,  pts=%I64d,frame pkt_pts=%I64d, pkt_dts=%I64d\n",pkt->pts, pkt->dts,*pts,frame->pkt_pts,frame->pkt_dts);

	printf("queue_picture video clock = %f\n",pts);


    /* update video clock for next frame */
	frame_delay = av_q2d(ps->video_ctx->time_base);
    /* for MPEG2, the frame can be repeated, so we update the
       clock accordingly */
    frame_delay += src_frame->repeat_pict * (frame_delay * 0.5);
    ps->video_clock += frame_delay;

	printf("video clock=%f pts=%f\n", ps->video_clock,pts);

#if defined(DEBUG_SYNC) && 0
    printf("frame_type=%c clock=%0.3f pts=%0.3f\n",
           av_get_picture_type_char(src_frame->pict_type), pts, pts1);
#endif

    /* wait until we have space to put a new picture */
    SDL_LockMutex(ps->pictq_mutex);

    /* keep the last already displayed picture in the queue */
    while (ps->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE - 2 &&
           !ps->videoq.abort_request) {
        SDL_CondWait(ps->pictq_cond, ps->pictq_mutex);
    }
    SDL_UnlockMutex(ps->pictq_mutex);

    if (ps->videoq.abort_request)
        return -1;

    vp = &ps->pictq[ps->pictq_windex];


 //   vp->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, src_frame);


    /* alloc or resize hardware picture buffer */
    if (!vp->bmp || vp->reallocate || !vp->allocated ||
        vp->width  != src_frame->width ||
        vp->height != src_frame->height) {
        SDL_Event event;

        vp->allocated  = 0;
        vp->reallocate = 0;
		vp->width = src_frame->width;
		vp->height = src_frame->height;

        /* the allocation must be done in the main thread to avoid
           locking problems. */
        event.type = FF_ALLOC_EVENT;
        event.user.data1 = ps;
        SDL_PushEvent(&event);

        /* wait until the picture is allocated */
        SDL_LockMutex(ps->pictq_mutex);
        while (!vp->allocated && !ps->videoq.abort_request) {
            SDL_CondWait(ps->pictq_cond, ps->pictq_mutex);
        }
        /* if the queue is aborted, we have to pop the pending ALLOC event or wait for the allocation to complete */
        if (ps->videoq.abort_request && SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENTMASK(FF_ALLOC_EVENT)) != 1) {
            while (!vp->allocated) {
                SDL_CondWait(ps->pictq_cond, ps->pictq_mutex);
            }
        }
        SDL_UnlockMutex(ps->pictq_mutex);

        if (ps->videoq.abort_request)
            return -1;
    }

    /* if the frame is not skipped, then display it */
    if (vp->bmp) {
        AVPicture pict = { { 0 } };

        /* get a pointer on the bitmap */
        SDL_LockYUVOverlay (vp->bmp);

        pict.data[0] = vp->bmp->pixels[0];
        pict.data[1] = vp->bmp->pixels[2];
        pict.data[2] = vp->bmp->pixels[1];

        pict.linesize[0] = vp->bmp->pitches[0];
        pict.linesize[1] = vp->bmp->pitches[2];
        pict.linesize[2] = vp->bmp->pitches[1];

 //       sws_flags = av_get_int(sws_opts, "sws_flags", NULL);
        ps->img_convert_ctx = sws_getCachedContext(ps->img_convert_ctx,
            vp->width, vp->height, (PixelFormat)src_frame->format, vp->width, vp->height,
            PIX_FMT_YUV420P, sws_flags, NULL, NULL, NULL);
        if (ps->img_convert_ctx == NULL) {
            fprintf(stderr, "Cannot initialize the conversion context\n");
            exit(1);
        }
        if( sws_scale(ps->img_convert_ctx, src_frame->data, src_frame->linesize,
                  0, vp->height, pict.data, pict.linesize) <= 0)
		{
			        /* update the bitmap content */
			 SDL_UnlockYUVOverlay(vp->bmp);
			 return -1;
		}


        /* update the bitmap content */
        SDL_UnlockYUVOverlay(vp->bmp);

		vp->pts = pts;
        vp->pos = pos;
        vp->skip = 0;

        /* now we can update the picture count */
        if (++ps->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE)
            ps->pictq_windex = 0;
        SDL_LockMutex(ps->pictq_mutex);
        ps->pictq_size++;
        SDL_UnlockMutex(ps->pictq_mutex);
    }
    return 0;
}

static int get_video_frame(PlayerState *ps, AVFrame *frame, int64_t *pts, AVPacket *pkt)
{
    int got_picture, i;

	if (ps->videoq.nb_packets <= 0) {
        avcodec_flush_buffers(ps->video_ctx);

        SDL_LockMutex(ps->pictq_mutex);
        // Make sure there are no long delay timers (ideally we should just flush the que but thats harder)
        for (i = 0; i < VIDEO_PICTURE_QUEUE_SIZE; i++) {
            ps->pictq[i].skip = 1;
        }
        while (ps->pictq_size && !ps->videoq.abort_request) {
            SDL_CondWait(ps->pictq_cond, ps->pictq_mutex);
        }

		ps->video_current_pos = -1;
        ps->frame_last_pts = AV_NOPTS_VALUE;
        ps->frame_last_duration = 0;
        ps->frame_timer = (double)av_gettime() / 1000000.0;
        ps->frame_last_dropped_pts = AV_NOPTS_VALUE;

        SDL_UnlockMutex(ps->pictq_mutex);

        return 0;
    }

    if (packet_queue_get(&ps->videoq, pkt, 1) < 0)
        return -1;

	//*static int counter = 0;

	/*	char log[128];
		sprintf(log,"#############pkt %d size =%d\n ",counter++,pkt->size);
*/
		//FILE *vfile = fopen("video.264","ab");
		//fwrite(pkt->data,1,pkt->size,vfile);
		//fclose(vfile);

    if(avcodec_decode_video2(ps->video_ctx, frame, &got_picture, pkt) < 0)
        return 0;

    if (got_picture) {
        int ret = 1;

		
      if (decoder_reorder_pts == -1) {
            *pts = av_frame_get_best_effort_timestamp(frame);
        } else if (decoder_reorder_pts) {
            *pts = frame->pkt_pts;
        } else {
            *pts = frame->pkt_dts;
        }		
        
        if (*pts == AV_NOPTS_VALUE) {
            *pts = 0;
        }

		printf("video decode pkt pts=%d, dts=%d, frame pts=%d, pkt_pts=%d, pkt_dts=%d pts=%I64u\n",pkt->pts, pkt->dts,frame->pts,frame->pkt_pts,frame->pkt_dts, *pts);


		 if (((ps->av_sync_type == AV_SYNC_AUDIO_MASTER && ps->has_audio) || ps->av_sync_type == AV_SYNC_EXTERNAL_CLOCK) &&
             (framedrop>0 || (framedrop && ps->has_audio))) {
            SDL_LockMutex(ps->pictq_mutex);
            if (ps->frame_last_pts != AV_NOPTS_VALUE && *pts) {

             double  clockdiff = get_video_clock(ps) - get_master_clock(ps);

//                double dpts = av_q2d(ps->audio_fmtctx->streams[0]->time_base) * *pts;

			 double dpts = av_q2d(ps->video_ctx->time_base) * *pts;

                double ptsdiff = dpts - ps->frame_last_pts;

				printf("get video frame clockdiff=%f, ptsdiff=%f\n",clockdiff, ptsdiff);

                if (fabs(clockdiff) < AV_NOSYNC_THRESHOLD &&
                     ptsdiff > 0 && ptsdiff < AV_NOSYNC_THRESHOLD &&
                     clockdiff + ptsdiff - ps->frame_last_filter_delay < 0) {
                    ps->frame_last_dropped_pos = pkt->pos;
                    ps->frame_last_dropped_pts = dpts;
                    ps->frame_drops_early++;
                    ret = 0;
                }
            }

			 SDL_UnlockMutex(ps->pictq_mutex);
        }
        return ret;
    }
    return 0;
}

static int video_thread(void *arg)
{
    AVPacket pkt = { 0 };
    PlayerState *ps = (PlayerState*) arg;
    AVFrame *frame = avcodec_alloc_frame();
    int64_t pts_int = AV_NOPTS_VALUE, pos = -1;
    double pts = 0;
    int ret;                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          

    for (;;) {

        while (ps->isPaused && !ps->videoq.abort_request)
            SDL_Delay(10);

        avcodec_get_frame_defaults(frame);
        av_free_packet(&pkt);

        ret = get_video_frame(ps, frame, &pts_int, &pkt);
        if (ret < 0)
            goto the_end;

        if (!ret)
		{
			SDL_Delay(10);
            continue;
		}

//		pts = pts_int * av_q2d(ps->audio_fmtctx->streams[0]->time_base);
		pts = pts_int * av_q2d(ps->video_ctx->time_base);

//		printf("video time_base=%f\n",av_q2d(ps->audio_fmtctx->streams[0]->time_base));

        ret = queue_picture(ps, frame, pts, pkt.pos);


        if (ret < 0)
            goto the_end;      
    }
 the_end:
    avcodec_flush_buffers(ps->video_ctx); 

    av_free_packet(&pkt);
    avcodec_free_frame(&frame);
    return 0;
}

 int player_prepare_video(PlayerState *player, VideoParams &vprams, const char *extradata, int extrasize)
{
	player->video_prams = vprams;

//		char *filename = "I:\\vc\\ffplay\\Debug\\out.264";
////	char *filename = "rtsp://10.214.8.81:8554/h264ESVideoTest";
//	AVFormatContext *avfmtctx = NULL;
//	int ret = avformat_open_input(&avfmtctx,filename,NULL,NULL);
//	if(ret < 0)
//	{
//		LOG("open file failed!\n");
//		return ret;
//	}
//
//	av_dump_format(avfmtctx,0,filename,false);
//
//	ret = avformat_find_stream_info(avfmtctx,NULL)  ;
//    if(ret < 0)
//	{
//		return ret;
//	}
//
//
//	 int videostream_index = -1;
//	 for(int j=0; j<avfmtctx->nb_streams; j++)//找到音频对应的stream
//	 {
//        if(avfmtctx->streams[j]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
//        {
//            videostream_index=j;
//            break;
//        }
//	 }
//	 player->audio_fmtctx = avfmtctx;

	AVCodec *video_cdc = avcodec_find_decoder(vprams.video_codec_id);
	if(!video_cdc)
	{
		LOG("can not find decoder!\n");
		goto fail;
	}

	 player->video_ctx = avcodec_alloc_context3(video_cdc);
     if( !player->video_ctx ) 
	 {
            LOG("Failed to alloc video context");
            goto fail;
      }

	

//	player->video_ctx = player->audio_fmtctx->streams[0]->codec;
//	video_cdc = avcodec_find_decoder(player->video_ctx->codec_id);
	
	player->video_ctx->pix_fmt = PIX_FMT_YUV420P;
	//player->video_ctx->idct_algo         = FF_IDCT_AUTO;
 //   player->video_ctx->skip_frame        = AVDISCARD_DEFAULT;
 //   player->video_ctx->skip_idct         = AVDISCARD_DEFAULT;
 //   player->video_ctx->skip_loop_filter  = AVDISCARD_DEFAULT;
 //   player->video_ctx->error_concealment = 3;
	player->video_ctx->time_base.den = 20;
	player->video_ctx->time_base.num = 1;
	player->video_ctx->width = 1280;
	player->video_ctx->height = 720;
	//player->video_ctx->coded_width = 1280;
	//player->video_ctx->coded_height = 720;
	player->video_ctx->gop_size = 10;
	player->video_ctx->me_method = 5;
	//player->video_ctx->bit_rate = 0;
	
	player->video_ctx->codec_type = AVMEDIA_TYPE_VIDEO;

//	AVDictionary *opts = NULL;  
//	av_dict_set(&opts, "profile", "baseline", 0); 
//	player->video_ctx->flags |= CODEC_FLAG_GLOBAL_HEADER;
	 ;

	//player->video_ctx->delay = 8;
	//player->video_ctx->min_prediction_order = -1;
	//player->video_ctx->max_prediction_order = -1;
	//player->video_ctx->thread_count = 9;
	//player->video_ctx->active_thread_type = 1;
	//player->video_ctx->profile = 100;
	//player->video_ctx->level = 31;
	//player->video_ctx->pkt_timebase.num = 1;
	//player->video_ctx->pkt_timebase.den = 1200000;


	 player->video_ctx->extradata = new uint8_t[extrasize];//给extradata成员参数分配内存
     player->video_ctx->extradata_size = extrasize;//extradata成员参数分配内存大小
	 memcpy(player->video_ctx->extradata,extradata,extrasize);

 //

	////给extradata成员参数设置值
	// //00 00 00 01 
	// player->video_ctx->extradata[0] = 0x00;
	// player->video_ctx->extradata[1] = 0x00;
	// player->video_ctx->extradata[2] = 0x00;
	// player->video_ctx->extradata[3] = 0x01;

	// //67 4D 40 33 92 54 0C 04 B4 20 00 00    03 00 40 00 00 0C D1 E3 06 54
	// //67 42 80 1e 
	// player->video_ctx->extradata[4] = 0x67;
	// player->video_ctx->extradata[5] = 0x4D;
	// player->video_ctx->extradata[6] = 0x40;
	// player->video_ctx->extradata[7] = 0x33;

	// //88 8b 40 50 
	// player->video_ctx->extradata[8] = 0x92;
	// player->video_ctx->extradata[9] = 0x54;
	// player->video_ctx->extradata[10] = 0x0C;
	// player->video_ctx->extradata[11] = 0x04;

	// //1e d0 80 00 
	//player->video_ctx->extradata[12] = 0xB4;
	//player->video_ctx->extradata[13] = 0x20;
	//player->video_ctx->extradata[14] = 0x00;
	//player->video_ctx->extradata[15] = 0x00;

	// //03 84 00 00 
	//player->video_ctx->extradata[16] = 0x03;
	//player->video_ctx->extradata[17] = 0x00;
	//player->video_ctx->extradata[18] = 0x40;
	//player->video_ctx->extradata[19] = 0x00;

	// //af c8 02 00 
	// player->video_ctx->extradata[20] = 0x00;
	// player->video_ctx->extradata[21] = 0x0C;
	// player->video_ctx->extradata[22] = 0xD1;
	// player->video_ctx->extradata[23] = 0xE3;

	// player->video_ctx->extradata[24] = 0x06;
	// player->video_ctx->extradata[25] = 0x54;
	// //00 00 00 01 
	// player->video_ctx->extradata[26] = 0x00;
	// player->video_ctx->extradata[27] = 0x00;
	// player->video_ctx->extradata[28] = 0x00;
	// player->video_ctx->extradata[29] = 0x01;

	// //68 ce 38 80
	// player->video_ctx->extradata[30] = 0x68;
	// player->video_ctx->extradata[31] = 0xee;
	// player->video_ctx->extradata[32] = 0x3c;
	// player->video_ctx->extradata[33] = 0x80;
	
	player->video_ctx->pix_fmt = vprams.tgt_fmt;
	//player->video_ctx->width = 256;
	//player->video_ctx->height = 256;
	

	 if( avcodec_open2(player->video_ctx, video_cdc, NULL) < 0)
	 {
		 LOG("failed to open decoder!\n");
		 goto fail;
	 }

	// for(int k =0; k < extrasize; k++)
	//{
	//	printf("%2x ",(unsigned char)extradata[k]);
	//}
	//printf("\n");

	 packet_queue_init(&player->videoq);


	 return 0;
fail:
	 if(player->video_ctx)
	 {
		 avcodec_close(player->video_ctx);
		 av_free(player->video_ctx);
		 player->video_ctx = NULL;
	 }

	 return -1;
}

static void toggle_full_screen(PlayerState *is)
{
#if defined(__APPLE__) && SDL_VERSION_ATLEAST(1, 2, 14)
    /* OS X needs to reallocate the SDL overlays */
    int i;
    for (i = 0; i < VIDEO_PICTURE_QUEUE_SIZE; i++)
        is->pictq[i].reallocate = 1;
#endif
    is->is_full_screen = !is->is_full_screen;
    video_open(is, 1);
}

static void stream_toggle_pause(PlayerState *is)
{
    if (is->isPaused) {
        is->frame_timer += av_gettime() / 1000000.0 + is->video_current_pts_drift - is->video_current_pts;

        is->video_current_pts_drift = is->video_current_pts - av_gettime() / 1000000.0;
    }
    is->isPaused = !is->isPaused;
}


 int player_start_audio(PlayerState *player)
{	
	packet_queue_start(&player->audioq);
	memset(&player->audio_pkt, 0, sizeof(player->audio_pkt));
    memset(&player->audio_pkt_temp, 0, sizeof(player->audio_pkt_temp));

	int audio_hw_buf_size = sdl_audio_open(player,player->audio_params_tgt.channel_layout,player->audio_params_tgt.channels,player->audio_params_tgt.freq, &player->audio_params_src);
	if (audio_hw_buf_size < 0)
          return -1;
    player->audio_hw_buf_size = audio_hw_buf_size;
	player->has_audio = 1;	
	SDL_PauseAudio(0);
	return 0;
	
}

static int refresh_thread(void *opaque)
{
    PlayerState *is= (PlayerState*) opaque;
    while (!is->request_abort) {
        SDL_Event event;
        event.type = FF_REFRESH_EVENT;
        event.user.data1 = opaque;
        if (!is->refresh && (!is->isPaused || is->force_refresh)) {
            is->refresh = 1;
            SDL_PushEvent(&event);
        }
        //FIXME ideally we should wait the correct time but SDLs event passing is so slow it would be silly
        av_usleep( 5000);
    }
    return 0;
}

 int player_start_video(PlayerState *player)
{
	 /* start video display */
    player->pictq_mutex = SDL_CreateMutex();
    player->pictq_cond  = SDL_CreateCond();
	player->pictq_size = 0;
	player->pictq_rindex = player->pictq_windex = 0;

	packet_queue_start(&player->videoq);
	player->video_tid = SDL_CreateThread(video_thread, player);
	player->refresh_tid = SDL_CreateThread(refresh_thread, player);
	player->has_video = 1;

	return 0;
}


//* handle an event sent by the GUI 
 void event_loop(PlayerState *cur_stream)
{
    SDL_Event event;
    double incr, pos, frac;

    for (;;) {
        double x;
        SDL_WaitEvent(&event);
//		SDL_PollEvent(&event);
        switch (event.type) {   
			
			case FF_ALLOC_EVENT:
				 alloc_picture((PlayerState *)event.user.data1);
            break;
		    case FF_REFRESH_EVENT:
				 video_refresh(event.user.data1);
				cur_stream->refresh = 0;
            break;
			 case SDL_VIDEORESIZE:
                cur_stream->screen = SDL_SetVideoMode(event.resize.w, event.resize.h, 0,
                                          SDL_HWSURFACE|SDL_RESIZABLE|SDL_ASYNCBLIT|SDL_HWACCEL);
                cur_stream->screen_width  = cur_stream->width  = event.resize.w;
                cur_stream->screen_height = cur_stream->height = event.resize.h;
                cur_stream->force_refresh = 1;
            break;

		   case SDL_KEYDOWN:
            
				switch (event.key.keysym.sym)
				{
					case SDLK_ESCAPE:
					case SDLK_q:
						do_exit(cur_stream);
						break;
					case SDLK_f:
                        toggle_full_screen(cur_stream);
						cur_stream->force_refresh = 1;
						break;
					case SDLK_p:
					case SDLK_SPACE:
						 stream_toggle_pause(cur_stream);
						break;
				}
        default:
            break;
        }
    }
}

bool ctrlhandler( DWORD fdwctrltype ) 
{ 
    switch( fdwctrltype ) 
    { 
    // handle the ctrl-c signal. 
    case CTRL_C_EVENT: 
        printf( "ctrl-c event\n\n" );
        return( true );
 
    // ctrl-close: confirm that the user wants to exit. 
    case CTRL_CLOSE_EVENT: 
        printf( "ctrl-close event\n\n" );
		
        return( true ); 
 
    // pass other signals to the next handler. 
    case CTRL_BREAK_EVENT: 
        printf( "ctrl-break event\n\n" );
        return false; 
 
    case CTRL_LOGOFF_EVENT: 
        printf( "ctrl-logoff event\n\n" );
        return false; 
 
    case CTRL_SHUTDOWN_EVENT: 
        printf( "ctrl-shutdown event\n\n" );
        return false; 
 
    default: 
        return false; 
    } 
} 
 

int player_start(PlayerState *player)
{
	player->request_abort = 0;
	player->isPaused = false;


	//player->continue_read_thread = SDL_CreateCond();
	//player->read_wait_mutex = SDL_CreateMutex();
	//player->read_tid = SDL_CreateThread(read_thread, player);

//	player_start_audio(player);
//	player_start_video(player);
	
	return 0;
}

static int do_exit(PlayerState *ps)
{
	player_stop(ps);
	player_destroy(ps);    
    av_log(NULL, AV_LOG_QUIET, "%s", "");
    exit(0);
}




//AVFormatContext *fmt_ctx=NULL;  
//AVCodecContext *codec_ctx;  
//AVCodec *codec;  
//AVPacket packet;  
//AVFrame *frame;  
//int frame_ptr;  
//#define mylog printf
//  
//int main(int argc, char **argv)
//{  
//    avcodec_register_all();  
//    av_register_all();  
//    if(avformat_open_input(&fmt_ctx,"I:\\vc\\ffplay\\Debug\\salt.mp3",NULL,NULL)!=0)  
//        {mylog("errno %d",errno);}  
//    if(avformat_find_stream_info(fmt_ctx,NULL)<0)  
//        {mylog("errno %d",errno);}  
//    codec_ctx=fmt_ctx->streams[0]->codec;  
//    codec = avcodec_find_decoder(codec_ctx->codec_id);  
//    mylog("id=%d\n",codec->id);  
//    if (avcodec_open2(codec_ctx, codec,NULL) < 0)  
//        {mylog("errno %d\n",errno);}  
//    while(av_read_frame(fmt_ctx,&packet)==0){  
//        frame=avcodec_alloc_frame();  
//        avcodec_decode_audio4(codec_ctx, frame, &frame_ptr,&packet);  
//        mylog("frame_ptr %d\n",frame_ptr);  
//        if(frame_ptr){  
//            int data_size = av_samples_get_buffer_size(frame->linesize,codec_ctx->channels,frame->nb_samples,codec_ctx->sample_fmt, 0);  
//            mylog("data count %d\n",data_size);  
//            /*int i=0; 
//            for(;i<data_size;++i){ 
//                mylog("data %d",(frame->data[0])[i]); 
//            }*/  
//        }  
//        av_free_packet(&packet);  
//    }  
//    avformat_close_input(&fmt_ctx);  
//
//	return 0;
//} 


//int main(int argc, char **argv)  
//{  
//    AVFormatContext *pFormatCtx = NULL;  
//    int             i, videoStream;  
//    AVCodecContext  *pCodecCtx;  
//    AVCodec         *pCodec;  
//    AVFrame         *pFrame;   
//    AVFrame         *pFrameRGB;  
//    AVPacket        packet;  
//    int             frameFinished;  
//    int             numBytes;  
//    uint8_t         *buffer;  
//    struct SwsContext *img_convert_ctx;  
//	char *filename = "I:\\vc\\ffplay\\Debug\\blood.mp4";
//  
//    //if(argc < 2)  
//    //{  
//    //    printf("Please provide a movie file\n");  
//    //    return -1;  
//    //}  
//    // Register all formats and codecs  
//    // 初始化ffmpeg库  
//
//    avcodec_register_all();  
//
//    av_register_all();  
//
//	int flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
////   
////    if (SDL_Init (flags)) {
////        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
////        fprintf(stderr, "(Did you set the DISPLAY variable?)\n");
////        exit(1);
////    }
////
////	SDL_EventState(SDL_ACTIVEEVENT, SDL_IGNORE);
////    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
////    SDL_EventState(SDL_USEREVENT, SDL_IGNORE);
//  
//    // Open video file  
//    if(avformat_open_input(&pFormatCtx, filename,NULL,NULL) < 0)  
//        return -1; // Couldn't open file  
//  
//    // Retrieve stream information  
//    // 查找文件的流信息  
//    if(av_find_stream_info(pFormatCtx)<0)  
//        return -1; // Couldn't find stream information  
//  
//    // Dump information about file onto standard error  
//    // dump只是一个调试函数，输出文件的音、视频流的基本信息：帧率、分辨率、音频采样等等  
////    dump_format(pFormatCtx, 0, argv[1], 0);  
//    av_dump_format(pFormatCtx,0,filename,false);
//
//    // Find the first video stream  
//    // 遍历文件的流，找到第一个视频流，并记录流的编码信息  
//    videoStream=-1;  
//    for(i=0; i<pFormatCtx->nb_streams; i++)  
//    {  
//        if(pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_VIDEO)  
//        {  
//            videoStream=i;  
//            break;  
//        }  
//    }  
//    if(videoStream==-1)  
//        return -1; // Didn't find a video stream  
//  
//    // Get a pointer to the codec context for the video stream  
//    // 得到视频流编码的上下文指针  
//    pCodecCtx=pFormatCtx->streams[videoStream]->codec;  
//  
//    // construct the scale context, conversing to PIX_FMT_RGB24  
//    // 根据编码信息设置渲染格式  
//    img_convert_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt,   
//            pCodecCtx->width, pCodecCtx->height, PIX_FMT_RGB24, SWS_BICUBIC, NULL, NULL, NULL);  
//    if(img_convert_ctx == NULL)  
//    {  
//        fprintf(stderr, "Cannot initialize the conversion context!\n");  
////      exit(1);  
//        return -1;  
//    }  
//  
//    // Find the decoder for the video stream  
//    // 在库里面查找支持该格式的解码器  
//    pCodec=avcodec_find_decoder(pCodecCtx->codec_id);  
//    if(pCodec==NULL)  
//    {  
//        fprintf(stderr, "Unsupported codec!\n");  
//        return -1; // Codec not found  
//    }  
//    // Open codec  
//    // 打开解码器  
//    if(avcodec_open2(pCodecCtx, pCodec,NULL)<0)  
//        return -1; // Could not open codec  
//  
//    // Allocate video frame  
//    // 分配一个帧指针，指向解码后的原始帧  
//    pFrame=avcodec_alloc_frame();  
//  
//    // Allocate an AVFrame structure  
//    // 分配一个帧指针，指向存放转换成rgb后的帧  
//    pFrameRGB=avcodec_alloc_frame();  
//    if(pFrameRGB==NULL)  
//        return -1;  
//  
//    // Determine required buffer size and allocate buffer  
//    numBytes=avpicture_get_size(PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height);  
//    buffer=(uint8_t *)av_malloc(numBytes*sizeof(uint8_t));  // buffer = new uint8_t[numBytes];  
//  
//    // Assign appropriate parts of buffer to image planes in pFrameRGB  
//    // Note that pFrameRGB is an AVFrame, but AVFrame is a superset  
//    // of AVPicture  
//    // 给pFrameRGB帧附加上分配的内存  
//    avpicture_fill((AVPicture *)pFrameRGB, buffer, PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height);  
//  
//    // Read frames and save first five frames to disk  
//    i=0;  
//	
//    while(av_read_frame(pFormatCtx, &packet)>=0) // 读取一个帧  
//    {  
//        // Is this a packet from the video stream?  
//        if(packet.stream_index==videoStream)  
//        {  
//            // Decode video frame  
//            // 解码该帧  
//			
//			printf("XXX packet pts=%I64d, dts =%I64d\n", packet.pts, packet.dts);
//            avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished,&packet);  
//  
//            // Did we get a video frame?  
//            if(frameFinished)  
//            {  
//                // Convert the image from its native format to RGB  
//                // img_convert((AVPicture *)pFrameRGB, PIX_FMT_RGB24,   
//                //              (AVPicture*)pFrame, pCodecCtx->pix_fmt, pCodecCtx->width,   
//                //              pCodecCtx->height);  
//				printf("*** frame pts=%d, pkt_pts=%I64d, pkt_dts =%I64d\n", pFrame->pts, pFrame->pkt_dts,pFrame->pkt_dts);   
////               printf("decode frame success!\n");
//   
//            }  
//        }  
//  
//        // Free the packet that was allocated by av_read_frame  
//        // 释放读取的帧内存  
//        av_free_packet(&packet); 
//		SDL_Delay(1000);
//    }  
//  
//    // Free the RGB image  
//    av_free(buffer);  
//    av_free(pFrameRGB);  
//  
//    // Free the YUV frame  
//    av_free(pFrame);  
//  
//    // Close the codec  
//    avcodec_close(pCodecCtx);  
//  
//    // Close the video file  
//    av_close_input_file(pFormatCtx);  
//  
//    return 0;  
//}  









