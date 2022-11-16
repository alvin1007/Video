#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/time.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

#include <SDL.h>
#include <SDL_image.h>

typedef struct Video
{
	AVFormatContext* ctx;
	AVCodecContext* codec_ctx;
	AVCodec* codec;
	struct SwsContext* sws_ctx;
	AVFrame* frame;

	SDL_Texture* t;

	int videoStream;
	int videoFinished;
} Video;

void DecodeFrame(Video* v)
{
	v->frame = av_frame_alloc();

	for (;;)
	{
		int ret;

		AVPacket* pkt = av_packet_alloc();
		av_read_frame(v->ctx, pkt);

		if (!pkt->stream_index == v->videoStream)
		{
			av_packet_free(&pkt);
			continue;
		}

		ret = avcodec_send_packet(v->codec_ctx, pkt);

		ret = avcodec_receive_frame(v->codec_ctx, v->frame);

		if (ret == AVERROR(EAGAIN)) {
			continue;
		}

		av_packet_unref(pkt);

		break;
	}
}

int main(int argc, char argv[])
{
	Video* v = (Video *)malloc(sizeof(Video));
	if (!v)
		return -1;

	AVFormatContext* ctx = avformat_alloc_context();
	if (!ctx)
		return -1;
	v->ctx = ctx;

	if (avformat_open_input(&ctx, "D:\\source\\video\\ed.webm", NULL, NULL))
		return -1;


	if (avformat_find_stream_info(ctx, NULL))
		return -1;

	v->videoStream = -1;

	for (unsigned int i = 0; i < ctx->nb_streams; i++)
	{
		if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
			v->videoStream = i;
	}

	if (v->videoStream == -1)
		return -1;

	AVCodecContext* codec_ctx;
	AVCodec* codec;

	codec_ctx = avcodec_alloc_context3(NULL);
	if (!codec_ctx)
		return -1;

	if (avcodec_parameters_to_context(codec_ctx, ctx->streams[v->videoStream]->codecpar) < 0)
		return -1;

	codec_ctx->pkt_timebase = ctx->streams[v->videoStream]->time_base;

	codec = avcodec_find_decoder(codec_ctx->codec_id);
	if (!codec)
		return -1;

	codec_ctx->codec_id = codec->id;

	if (avcodec_open2(codec_ctx, codec, NULL))
		return -1;

	v->codec_ctx = codec_ctx;
	v->codec = codec;

	v->sws_ctx = sws_getContext(
		v->codec_ctx->width,
		v->codec_ctx->height,
		v->codec_ctx->pix_fmt,
		v->codec_ctx->width,
		v->codec_ctx->height,
		AV_PIX_FMT_YUV420P,
		SWS_BILINEAR,
		NULL,
		NULL,
		NULL
	);

	SDL_Window* window;
	SDL_Renderer* renderer;
	SDL_Event e;
	SDL_Rect render = { 0, 0, 1920, 1080 };
	SDL_Rect Print = { 0, 0, 1280, 720 };

	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		return -1;
	}

	window = SDL_CreateWindow("Test", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, SDL_WINDOW_SHOWN);
	renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	v->t = SDL_CreateTexture(
		renderer,
		SDL_PIXELFORMAT_YV12, 
		SDL_TEXTUREACCESS_STREAMING,
		v->codec_ctx->width,
		v->codec_ctx->height
	);

	for (;;)
	{
		while (SDL_PollEvent(&e) == 1) {}

		DecodeFrame(v);
		SDL_UpdateYUVTexture(
			v->t,
			&render,
			v->frame->data[0],
			v->frame->linesize[0],
			v->frame->data[1],
			v->frame->linesize[1],
			v->frame->data[2],
			v->frame->linesize[2]
		);

		SDL_RenderClear(renderer);
		SDL_RenderCopy(renderer, v->t, NULL, &Print);
		SDL_RenderPresent(renderer);

		SDL_Delay(20);
	}

	return 0;
}
