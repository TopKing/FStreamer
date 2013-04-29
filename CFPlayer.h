
#ifndef _CFPLAYER_H
#define _CFPLAYER_H


struct PlayerState;
struct VideoParams;
struct AudioParams;

//typedef struct PlayerState PlayerState;
//typedef struct VideoParams VideoParams;
//typedef struct AudioParams AudioParams;

 int player_start(PlayerState *player);
 int player_start_audio(PlayerState *player);
 int player_prepare_video(PlayerState *player,VideoParams &vprams,const char *extradata,int extrasize);
 int player_start_video(PlayerState *player);
 int player_prepare_audio_stream(PlayerState *player,struct AudioParams &src , struct AudioParams &tgt);
 void event_loop(PlayerState *ps);
 int player_stop(PlayerState *ps);
 int player_destroy(PlayerState *ps);


#endif