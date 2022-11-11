#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/time.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

#include <SDL.h>
#include <SDL_thread.h>

#define FRAME 5

const int ROW_ALIGNMENT = 16;
const int FRAME_PADDING = 4;

SDL_Surface* sample = NULL;

typedef struct PacketQueueEntry 
{
    AVPacket* pkt;
    struct PacketQueueEntry* next;
} PacketQueueEntry;

typedef struct PacketQueue
{
    PacketQueueEntry* first;
    PacketQueueEntry* last;
} PacketQueue;

typedef struct SurfaceQueueEntry 
{
    struct SurfaceQueueEntry* next;

    SDL_Surface* surf;

    double pts;

    SDL_PixelFormat* format;

    int w, h, pitch;
    void* pixels;
} SurfaceQueueEntry;

typedef struct Video 
{
    SDL_RWops* rwops;
    int quit;

    AVFrame* video_decode_frame;
    struct SwsContext* sws;
    SurfaceQueueEntry* surface_queue; // Lock
    int surface_queue_size; // Lock
    double video_pts_offset;
    double video_read_time;
    int frame_drops;
    double pause_time;
    double time_offset;
    int needs_decode;

    int video_finished;
    int video_stream;
    AVFormatContext* ctx;
    AVCodecContext* video_context;
    PacketQueue video_packet_queue;
} Video;

int rwops_read(void* opaque, uint8_t* buf, int buf_size) {
    SDL_RWops* rw = (SDL_RWops*)opaque;

    int rv = rw->read(rw, buf, 1, buf_size);
    return rv;

}

int rwops_write(void* opaque, uint8_t* buf, int buf_size) {
    printf("Writing to an SDL_rwops is a really bad idea.\n");
    return -1;
}

int64_t rwops_seek(void* opaque, int64_t offset, int whence) {
    SDL_RWops* rw = (SDL_RWops*)opaque;

    if (whence == AVSEEK_SIZE) {
        return rw->size(rw);
    }

    // Ignore flags like AVSEEK_FORCE.
    whence &= (SEEK_SET | SEEK_CUR | SEEK_END);

    int64_t rv = rw->seek(rw, (int)offset, whence);
    return rv;
}

void enqueue_packet(PacketQueue* pq, AVPacket* pkt) {
    PacketQueueEntry* pqe = av_malloc(sizeof(PacketQueueEntry));
    if (pqe == NULL) {
        av_packet_free(&pkt);
        return;
    }

    pqe->pkt = pkt;
    pqe->next = NULL;

    if (!pq->first) {
        pq->first = pq->last = pqe;
    }
    else {
        pq->last->next = pqe;
        pq->last = pqe;
    }
}

void dequeue_packet(PacketQueue* pq) {
    if (!pq->first) {
        return;
    }

    PacketQueueEntry* pqe = pq->first;
    pq->first = pqe->next;

    if (!pq->first) {
        pq->last = NULL;
    }

    av_packet_free(&pqe->pkt);
    av_free(pqe);
}

AVPacket* first_packet(PacketQueue* pq) {
    if (pq->first) {
        return pq->first->pkt;
    }
    else {
        return NULL;
    }
}

AVPacket* read_packet(Video* ms, PacketQueue* pq) {

    AVPacket* pkt;
    AVPacket* rv;

    while (1) {
        printf("h");

        rv = first_packet(pq);
        if (rv) {
            return rv;
        }
        pkt = av_packet_alloc();

        if (!pkt) {
            return NULL;
        }

        if (av_read_frame(ms->ctx, pkt)) {
            return NULL;
        }

        if (pkt->stream_index == ms->video_stream && !ms->video_finished) {
            
            enqueue_packet(&ms->video_packet_queue, pkt);
        }
        else {
            av_packet_free(&pkt);
        }
    }
}

enum AVPixelFormat get_pixel_format(SDL_Surface* surf) {
    uint32_t pixel;
    uint8_t* bytes = (uint8_t*)&pixel;

    printf("h");
    printf("h");
    pixel = SDL_MapRGBA(surf->format, 1, 2, 3, 4);
    printf("h");
    printf("h");

    enum AVPixelFormat fmt;

    if ((bytes[0] == 4 || bytes[0] == 0) && bytes[1] == 1) {
        fmt = AV_PIX_FMT_ARGB;
    }
    else if ((bytes[0] == 4 || bytes[0] == 0) && bytes[1] == 3) {
        fmt = AV_PIX_FMT_ABGR;
    }
    else if (bytes[0] == 1) {
        fmt = AV_PIX_FMT_RGBA;
    }
    else {
        fmt = AV_PIX_FMT_BGRA;
    }

    return fmt;
}

SurfaceQueueEntry* decode_video_frame(Video* ms) {
    int ret;

    while (1) {
        AVPacket* pkt = av_packet_alloc();
        av_read_frame(ms->ctx, pkt);

        if (pkt->stream_index == ms->video_stream && !ms->video_finished) {
            enqueue_packet(&ms->video_packet_queue, pkt);
        }
        else {
            av_packet_free(&pkt);
            continue;
        }

        ret = avcodec_send_packet(ms->video_context, pkt);

        if (ret == 0) {
            dequeue_packet(&ms->video_packet_queue);
        }
        else if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            // pass
        }
        else {
            ms->video_finished = 1;
            return NULL;
        }

        ret = avcodec_receive_frame(ms->video_context, ms->video_decode_frame);

        // More input is needed.
        if (ret == AVERROR(EAGAIN)) {
            continue;
        }

        if (ret < 0) {
            ms->video_finished = 1;
            return NULL;
        }

        break;
    }

    double pts = ms->video_decode_frame->best_effort_timestamp * av_q2d(ms->ctx->streams[ms->video_stream]->time_base);

    // If we're behind on decoding the frame, drop it.
    if (ms->video_pts_offset && (ms->video_pts_offset + pts < ms->video_read_time)) {

        // If we're 5s behind, give up on video for the time being, so we don't
        // blow out memory.
        if (ms->video_pts_offset + pts < ms->video_read_time - 5.0) {
            ms->video_finished = 1;
        }

        if (ms->frame_drops) {
            return NULL;
        }
    }

    ms->sws = sws_getCachedContext(
        ms->sws,

        ms->video_decode_frame->width,
        ms->video_decode_frame->height,
        ms->video_decode_frame->format,

        ms->video_decode_frame->width,
        ms->video_decode_frame->height,
        AV_PIX_FMT_RGBA,

        SWS_POINT,

        NULL,
        NULL,
        NULL
    );
    if (!ms->sws) {
        ms->video_finished = 1;
        return NULL;
    }

    SurfaceQueueEntry* rv = av_malloc(sizeof(SurfaceQueueEntry));
    if (rv == NULL) {
        ms->video_finished = 1;
        return NULL;
    }
    rv->w = ms->video_decode_frame->width + FRAME_PADDING * 2;
    rv->h = ms->video_decode_frame->height + FRAME_PADDING * 2;
    rv->pitch = rv->w * sample->format->BytesPerPixel;
    if (rv->pitch % ROW_ALIGNMENT) {
        rv->pitch += ROW_ALIGNMENT - (rv->pitch % ROW_ALIGNMENT);
    }
    rv->pixels = SDL_calloc(rv->pitch * rv->h, 1);

    rv->format = sample->format;
    rv->next = NULL;
    rv->pts = pts;

    uint8_t* surf_pixels = (uint8_t*)rv->pixels;
    uint8_t* surf_data[] = { &surf_pixels[FRAME_PADDING * rv->pitch + FRAME_PADDING * sample->format->BytesPerPixel] };
    int surf_linesize[] = { rv->pitch };
    sws_scale(
        ms->sws,

        (const uint8_t* const*)ms->video_decode_frame->data,
        ms->video_decode_frame->linesize,

        0,
        ms->video_decode_frame->height,

        surf_data,
        surf_linesize
    );

    return rv;
}

static void enqueue_surface(SurfaceQueueEntry** queue, SurfaceQueueEntry* sqe) {
    while (*queue) {
        queue = &(*queue)->next;
    }

    *queue = sqe;
}

int main(int argc, char* argv[]) 
{
    SDL_RWops* rwops = SDL_RWFromFile("D:\\program\\renpy\\SummerFlower_Mode\\game\\video\\lastwish.webm", "r");
    if (!rwops)
        return -1;

    Video* v = av_calloc(1, sizeof(Video));
    if (!v)
        return -1;

    AVFormatContext* ctx = avformat_alloc_context();
    if (!ctx)
        return -1;
    v->ctx = ctx;

    unsigned char* buffer = av_malloc(9999999);
    if (!buffer)
        return -1;

    AVIOContext* io_context = avio_alloc_context(
        buffer,
        9999999,
        0,
        rwops,
        rwops_read,
        rwops_write,
        rwops_seek);
    if (!io_context)
        return -1;
    ctx->pb = io_context;
    ctx->flags |= AVFMT_FLAG_CUSTOM_IO;

    if (avformat_open_input(&ctx, "D:\\program\\renpy\\SummerFlower_Mode\\game\\video\\lastwish.webm", NULL, NULL))
        return -1;

    if (avformat_find_stream_info(ctx, NULL))
        return -1;

    v->video_stream = -1;

    for (unsigned int i = 0; i < ctx->nb_streams; i++)
    {
        if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            v->video_stream = i;
    }

    AVDictionary* opts = NULL;
    AVCodec* codec = NULL;
    AVCodecContext* codec_ctx = NULL;

    codec_ctx = avcodec_alloc_context3(NULL);
    if (!codec_ctx)
        return -1;

    if (avcodec_parameters_to_context(codec_ctx, ctx->streams[v->video_stream]->codecpar) < 0)
        return -1;

    codec_ctx->pkt_timebase = ctx->streams[v->video_stream]->time_base;

    codec = avcodec_find_decoder(codec_ctx->codec_id);
    if (!codec)
        return -1;

    codec_ctx->codec_id = codec->id;

    av_dict_set(&opts, "refcounted_frames", "0", 0);

    if (avcodec_open2(codec_ctx, codec, &opts))
        return -1;

    v->video_context = codec_ctx;
    v->sws = sws_alloc_context();
    v->surface_queue_size = 0;
    v->video_finished = 0;
    sample = SDL_malloc(sizeof(SDL_Surface));
    // v->video_packet_queue = *(PacketQueue*)(av_malloc(sizeof(PacketQueue)));

    while (!v->video_finished)
    {  
        if (!v->video_decode_frame)
            v->video_decode_frame = av_frame_alloc();

        if (!v->video_decode_frame)
        {
            v->video_finished = 1;
            continue;
        }

        if (!v->video_finished && (v->surface_queue_size) < FRAME)
        {
            SurfaceQueueEntry* sqe = decode_video_frame(v);

            if (sqe)
            {
                enqueue_surface(&v->surface_queue, sqe);
                v->surface_queue_size += 1;
            }
        }
        else {
            break;
        }
    }

    printf("%d\n", v->surface_queue->w);
    printf("%d\n", v->surface_queue->next->w);
    printf("%d\n", v->surface_queue->next->next->w);
    printf("%d\n", v->surface_queue->next->next->next->w);
    printf("%d\n", v->surface_queue->next->next->next->next->w);
    printf("done");

    return 0;
}