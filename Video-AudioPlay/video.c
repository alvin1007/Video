#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include <SDL.h>
#include <SDL_mixer.h>
#include <SDL_thread.h>

#include <time.h>
#include <windows.h> 
#pragma comment(lib, "Winmm.lib")

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

	unsigned long starttime;
	double difftime;
	SDL_mutex* lock;

} Video;

void DecodeFrame(Video* v, int index)
{
	if (index < v->codec_ctx->frame_number)
		return;

	AVFrame* frame = av_frame_alloc();
		
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

		if (ret == AVERROR_EOF)
		{
			av_packet_unref(pkt);
			break;
		}

		ret = avcodec_receive_frame(v->codec_ctx, frame);

		if (ret == AVERROR(EAGAIN))
			continue;

		if (index > v->codec_ctx->frame_number)
		{
			av_packet_unref(pkt);
			continue;
		}

		av_packet_unref(pkt);

		break;
	}
	// AVFrame* del = v->frame;
	v->frame = frame;
}

int video_thread(void* data)
{
	Video* v = (Video*)data;
	SDL_Rect render = { 0, 0, 1920, 1080 };
	int delay = 1000 / 30;

	for(;;)
	{
		SDL_LockMutex(v->lock);
		DecodeFrame(v, (int)((timeGetTime() - v->starttime) / (1000.0 / 24.0)));

		// printf("%d %d\n", v->time.millitm, start.millitm);
	
		if (v->frame)
		{
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

			av_frame_free(&v->frame);
		}

		SDL_UnlockMutex(v->lock);
		
		SDL_Delay(20);
	}

	return 0;
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
	// D:\\video\\pentool_AME\\main_1.mp4
	// D:\\video\\clock_AME\\main_2.mp4
	// D:\\source\\video\\bg\\bg32.webm


	if (avformat_open_input(&ctx, "D:\\source\\ba.mp4", NULL, NULL))
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
	SDL_Rect print = { (1280 - 640) / 2, (720 - 360) / 2, 640, 360};
	int quit = 0;
	int x, y = 0;
	int nowClick = 0;
	int pts = 0;
	int delay = 0;
	// int fullscreen = 0;

	if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO|SDL_INIT_EVENTS) < 0) {
		return -1;
	}

	Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048);

	window = SDL_CreateWindow("Test", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, SDL_WINDOW_SHOWN);
	renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	Mix_Music* music = Mix_LoadMUS("D:\\source\\baa.ogg");
	v->t = SDL_CreateTexture(
		renderer,
		SDL_PIXELFORMAT_YV12, 
		SDL_TEXTUREACCESS_STREAMING,
		v->codec_ctx->width,
		v->codec_ctx->height
	);
	v->lock = SDL_CreateMutex();
	// SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);

	Mix_PlayMusic(music, 1);

	// SDL_Delay(500);

	v->starttime = timeGetTime();
	SDL_CreateThread(video_thread, "video_thread", v);
	// SDL_Delay(50);

	while (!quit)
	{
		while (SDL_PollEvent(&e) == 1)
		{
			switch (e.type)
			{
			case SDL_QUIT:
				quit = 1;
				break;
			case SDL_MOUSEBUTTONDOWN:
				SDL_GetMouseState(&x, &y);
				SDL_Rect t = { x - 320, y - 180, 640, 360 };
				print = t;
				nowClick = 1;
				break;
			case SDL_MOUSEMOTION:
				if (nowClick)
				{
					SDL_GetMouseState(&x, &y);
					SDL_Rect t = { x - 320, y - 180, 640, 360 };
					print = t;
				}
				break;
			case SDL_MOUSEBUTTONUP:
				nowClick = 0;
				break;
			case SDL_KEYDOWN:
				if (e.key.keysym.sym == SDLK_f)
					SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
			}
		}

		
		SDL_RenderClear(renderer);
		SDL_SetRenderDrawColor(renderer, 0xFF, 0xFF, 0xFF, 0xFF);

		SDL_LockMutex(v->lock);
		SDL_RenderCopy(renderer, v->t, NULL, NULL);
		SDL_UnlockMutex(v->lock);

		SDL_RenderPresent(renderer);
		
	}

	return 0;
}
