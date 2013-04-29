
#ifndef _PLAYER_QUEUE_H
#define _PLAYER_QUEUE_H

extern "C"
{
	#include "sdl/SDL.h"
	#include "libavformat/avformat.h"
}


typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    int abort_request;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;



 int packet_queue_put_private(PacketQueue *q, AVPacket *pkt);

 int packet_queue_put(PacketQueue *q, AVPacket *pkt);

/* packet queue handling */
 void packet_queue_init(PacketQueue *q);

 void packet_queue_flush(PacketQueue *q);

 void packet_queue_destroy(PacketQueue *q);

 void packet_queue_abort(PacketQueue *q);

 void packet_queue_start(PacketQueue *q);

 int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block);

  int packet_queue_is_started(PacketQueue *q);

#endif