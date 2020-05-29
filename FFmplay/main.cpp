
#include <cassert>
#include <string>
#include <codecvt>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <memory>

extern "C"
{
    #include "config.h"
    #include <inttypes.h>
    #include <math.h>
    #include <limits.h>
    #include <signal.h>
    #include <stdint.h>
    
    #include "libavutil/avstring.h"
    #include "libavutil/eval.h"
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
    
    #if CONFIG_AVFILTER
    # include "libavfilter/avfilter.h"
    # include "libavfilter/buffersink.h"
    # include "libavfilter/buffersrc.h"
    #endif
    
    #include <SDL.h>
    
    #include "cmdutils.h"
}

const char program_name[] = "ffplay";
const int program_birth_year = 2003;

constexpr uint32_t MAX_QUEUE_SIZE = (15 * 1024 * 1024);
constexpr uint32_t MIN_FRAMES = 25;
constexpr uint32_t EXTERNAL_CLOCK_MIN_FRAMES = 2;
constexpr uint32_t EXTERNAL_CLOCK_MAX_FRAMES = 10;

/* Minimum SDL audio buffer size, in samples. */
constexpr uint32_t SDL_AUDIO_MIN_BUFFER_SIZE = 512;
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
constexpr uint32_t SDL_AUDIO_MAX_CALLBACKS_PER_SEC = 30;

/* Step size for volume control in dB */
constexpr double SDL_VOLUME_STEP = (0.75);

/* no AV sync correction is done if below the minimum AV sync threshold */
constexpr double AV_SYNC_THRESHOLD_MIN = 0.04;
/* AV sync correction is done if above the maximum AV sync threshold */
constexpr double AV_SYNC_THRESHOLD_MAX = 0.1;
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
constexpr double AV_SYNC_FRAMEDUP_THRESHOLD = 0.1;
/* no AV correction is done if too big error */
constexpr double AV_NOSYNC_THRESHOLD = 10.0;

/* maximum audio speed change to get correct sync */
constexpr uint32_t SAMPLE_CORRECTION_PERCENT_MAX = 10;

/* external clock speed adjustment constants for realtime sources based on buffer fullness */
constexpr double EXTERNAL_CLOCK_SPEED_MIN = 0.900;
constexpr double EXTERNAL_CLOCK_SPEED_MAX = 1.010;
constexpr double EXTERNAL_CLOCK_SPEED_STEP = 0.001;

/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
constexpr uint32_t AUDIO_DIFF_AVG_NB = 20;

/* polls for possible required screen refresh at least this often, should be less than 1/fps */
constexpr double REFRESH_RATE = 0.01;

/* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
/* TODO: We assume that a decoded and resampled frame fits into this buffer */
constexpr uint32_t SAMPLE_ARRAY_SIZE = (8 * 65536);

constexpr uint32_t CURSOR_HIDE_DELAY = 1000000;

constexpr uint32_t USE_ONEPASS_SUBTITLE_RENDER = 1;

constexpr uint32_t VIDEO_PICTURE_QUEUE_SIZE = 3;
constexpr uint32_t SUBPICTURE_QUEUE_SIZE = 16;
constexpr uint32_t SAMPLE_QUEUE_SIZE = 9;
constexpr uint32_t FRAME_QUEUE_SIZE = FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE));

constexpr uint32_t FF_QUIT_EVENT = (SDL_USEREVENT + 2);

static unsigned sws_flags = SWS_BICUBIC;

static const struct TextureFormatEntry {
    enum AVPixelFormat format;
    int texture_fmt;
} sdl_texture_format_map[] = {
    { AV_PIX_FMT_RGB8,           SDL_PIXELFORMAT_RGB332 },
    { AV_PIX_FMT_RGB444,         SDL_PIXELFORMAT_RGB444 },
    { AV_PIX_FMT_RGB555,         SDL_PIXELFORMAT_RGB555 },
    { AV_PIX_FMT_BGR555,         SDL_PIXELFORMAT_BGR555 },
    { AV_PIX_FMT_RGB565,         SDL_PIXELFORMAT_RGB565 },
    { AV_PIX_FMT_BGR565,         SDL_PIXELFORMAT_BGR565 },
    { AV_PIX_FMT_RGB24,          SDL_PIXELFORMAT_RGB24 },
    { AV_PIX_FMT_BGR24,          SDL_PIXELFORMAT_BGR24 },
    { AV_PIX_FMT_0RGB32,         SDL_PIXELFORMAT_RGB888 },
    { AV_PIX_FMT_0BGR32,         SDL_PIXELFORMAT_BGR888 },
    { AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888 },
    { AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888 },
    { AV_PIX_FMT_RGB32,          SDL_PIXELFORMAT_ARGB8888 },
    { AV_PIX_FMT_RGB32_1,        SDL_PIXELFORMAT_RGBA8888 },
    { AV_PIX_FMT_BGR32,          SDL_PIXELFORMAT_ABGR8888 },
    { AV_PIX_FMT_BGR32_1,        SDL_PIXELFORMAT_BGRA8888 },
    { AV_PIX_FMT_YUV420P,        SDL_PIXELFORMAT_IYUV },
    { AV_PIX_FMT_YUYV422,        SDL_PIXELFORMAT_YUY2 },
    { AV_PIX_FMT_UYVY422,        SDL_PIXELFORMAT_UYVY },
    { AV_PIX_FMT_NONE,           SDL_PIXELFORMAT_UNKNOWN },
};

/* options specified by the user */
static AVInputFormat* file_iformat;
static const char* input_filename;

static const char* wanted_stream_spec[AVMEDIA_TYPE_NB] = { 0 };



static int decoder_reorder_pts = -1;

static int alwaysontop;
static int show_status = 1;

static int genpts = 0;
static int lowres = 0;
static int autoexit;
static int exit_on_keydown;
static int exit_on_mousedown;

#if CONFIG_AVFILTER
static const char** vfilters_list = nullptr;
static int nb_vfilters = 0;
static char* afilters = nullptr;
#endif
static int autorotate = 1;
static int find_stream_info = 1;
static int filter_nbthreads = 0;

static AVPacket flush_pkt;

typedef struct MyAVPacketList {
    AVPacket pkt;
    struct MyAVPacketList* next;
    int serial;
} MyAVPacketList;

class PacketQueue 
{
public:
    /* packet queue handling */
    int Init()
    {
        /*av_init_packet(&_flush_pkt);
        _flush_pkt.data = (uint8_t*)&_flush_pkt;*/

        abort_request = 1;
        return 0;
    }

    int PutPrivate(AVPacket* pkt)
    {
        MyAVPacketList* pkt1;

        if (abort_request)
            return -1;

        pkt1 = static_cast<MyAVPacketList*>(av_malloc(sizeof(MyAVPacketList)));
        if (!pkt1)
            return -1;
        pkt1->pkt = *pkt;
        pkt1->next = nullptr;
        if (pkt == &flush_pkt)
            serial++;
        pkt1->serial = serial;

        if (!last_pkt)
            first_pkt = pkt1;
        else
            last_pkt->next = pkt1;
        last_pkt = pkt1;
        nb_packets++;
        size += pkt1->pkt.size + sizeof(*pkt1);
        duration += pkt1->pkt.duration;
        /* XXX: should duplicate packet data in DV case */
        cond.notify_one();//SDL_CondSignal(cond);
        return 0;
    }

    int Put(AVPacket* pkt)
    {
        int ret;

        {
            std::lock_guard<std::mutex> lock(mutex);
            ret = PutPrivate(pkt);
        }

        if (pkt != &flush_pkt && ret < 0)
            av_packet_unref(pkt);

        return ret;
    }

    int PutNullPacket(int stream_index)
    {
        AVPacket pkt1, * pkt = &pkt1;
        av_init_packet(pkt);
        pkt->data = nullptr;
        pkt->size = 0;
        pkt->stream_index = stream_index;
        return Put(pkt);
    }

    void Flush()
    {
        MyAVPacketList* pkt, * pkt1;

        std::lock_guard<std::mutex> lock(mutex);
        for (pkt = first_pkt; pkt; pkt = pkt1) {
            pkt1 = pkt->next;
            av_packet_unref(&pkt->pkt);
            av_freep(&pkt);
        }
        last_pkt = nullptr;
        first_pkt = nullptr;
        nb_packets = 0;
        size = 0;
        duration = 0;
    }

    void Destroy()
    {
        Flush();
    }

    void Abort()
    {
        std::lock_guard<std::mutex> lock(mutex);
        abort_request = 1;

        cond.notify_one();
    }

    void Start()
    {
        std::lock_guard<std::mutex> lock(mutex);
        abort_request = 0;
        PutPrivate(&flush_pkt);
    }

    /* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
    int Get(AVPacket* pkt, int block, int* serial)
    {
        MyAVPacketList* pkt1 = nullptr;
        int ret;

        std::unique_lock<std::mutex> lock(mutex);

        for (;;) {
            if (abort_request) {
                ret = -1;
                break;
            }

            pkt1 = first_pkt;
            if (pkt1) {
                first_pkt = pkt1->next;
                if (!first_pkt)
                    last_pkt = nullptr;
                nb_packets--;
                size -= pkt1->pkt.size + sizeof(*pkt1);
                duration -= pkt1->pkt.duration;
                *pkt = pkt1->pkt;
                if (serial)
                    *serial = pkt1->serial;
                av_free(pkt1);
                ret = 1;
                break;
            }
            else if (!block) {
                ret = 0;
                break;
            }
            else {
                cond.wait(lock);
            }
        }

        return ret;
    }
    MyAVPacketList* first_pkt = nullptr, * last_pkt = nullptr;
    //AVPacket _flush_pkt;
    int nb_packets = 0;
    int size = 0;
    int64_t duration = 0;
    int abort_request = false;
    int serial;
    std::mutex mutex;
    std::condition_variable cond;
};

struct AudioParams {
    int freq;
    int channels;
    int64_t channel_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
};

class SClock 
{
public:
    void Init(int* queue_serial)
    {
        speed = 1.0;
        paused = 0;
        this->queue_serial = queue_serial;
        Set(NAN, -1);
    }

    double Get()
    {
        if (*queue_serial != serial)
            return NAN;
        if (paused) {
            return pts;
        }
        else {
            double time = av_gettime_relative() / 1000000.0;
            return pts_drift + time - (time - last_updated) * (1.0 - speed);
        }
    }

    void SetAt(double pts, int serial, double time)
    {
        pts = pts;
        last_updated = time;
        pts_drift = pts - time;
        serial = serial;
    }

    void Set(double pts, int serial)
    {
        double time = av_gettime_relative() / 1000000.0;
        SetAt(pts, serial, time);
    }

    void SetSpeed(double speed)
    {
        Set(Get(), serial);
        speed = speed;
    }

public:
    double pts;           /* clock base */
    double pts_drift;     /* clock base minus time at which we updated the clock */
    double last_updated;
    double speed;
    int serial;           /* clock is based on a packet with this serial */
    int paused;
    int* queue_serial = nullptr;    /* pointer to the current packet queue serial, used for obsolete clock detection */
};

/* Common struct for handling all types of decoded data and allocated render buffers. */
struct SFrame 
{
    AVFrame* frame = nullptr;
    AVSubtitle sub;
    int serial;
    double pts;           /* presentation timestamp for the frame */
    double duration;      /* estimated duration of the frame */
    int64_t pos;          /* byte position of the frame in the input file */
    int width;
    int height;
    int format;
    AVRational sar;
    int uploaded;
    int flip_v;
};

class FrameQueue 
{
public:
    int Init(PacketQueue* pktq, int max_size, int keep_last)
    {
        int i;
        this->pktq = pktq;
        this->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
        this->keep_last = !!keep_last;
        for (i = 0; i < max_size; i++)
            if (!(queue[i].frame = av_frame_alloc()))
                return AVERROR(ENOMEM);
        return 0;
    }

    void Destory()
    {
        int i;
        for (i = 0; i < max_size; i++) {
            SFrame* vp = &queue[i];
            UnrefItem(vp);
            av_frame_free(&vp->frame);
        }
    }

    void Signal()
    {
        std::lock_guard<std::mutex> lock(mutex);
        cond.notify_one();
    }

    SFrame* Peek()
    {
        return &queue[(rindex + rindex_shown) % max_size];
    }

    SFrame* PeekNext()
    {
        return &queue[(rindex + rindex_shown + 1) % max_size];
    }

    SFrame* PeekLast()
    {
        return &queue[rindex];
    }

    SFrame* PeekWritable()
    {
        /* wait until we have space to put a new frame */
        {
            std::unique_lock<std::mutex> lock(mutex);
            while (size >= max_size &&
                !pktq->abort_request) {
                cond.wait(lock);
            }
        }

        if (pktq->abort_request)
            return nullptr;

        return &queue[windex];
    }

    SFrame* PeekReadable()
    {
        /* wait until we have a readable a new frame */
        {
            std::unique_lock<std::mutex> lock(mutex);
            while (size - rindex_shown <= 0 &&
                !pktq->abort_request) {
                cond.wait(lock);
            }
        }

        if (pktq->abort_request)
            return nullptr;

        return &queue[(rindex + rindex_shown) % max_size];
    }

    void Push()
    {
        if (++windex == max_size)
            windex = 0;

        std::lock_guard<std::mutex> lock(mutex);
        size++;
        cond.notify_one();
    }

    void Next()
    {
        if (keep_last && !rindex_shown) 
        {
            rindex_shown = 1;
            return;
        }
        UnrefItem(&queue[rindex]);
        if (++rindex == max_size)
            rindex = 0;

        std::lock_guard<std::mutex> lock(mutex);
        size--;
        cond.notify_one();
    }

    /* return the number of undisplayed frames in the queue */
    int NbRemaining()
    {
        return size - rindex_shown;
    }

    /* return last shown position */
    int64_t LastPos()
    {
        SFrame* fp = &queue[rindex];
        if (rindex_shown && fp->serial == pktq->serial)
            return fp->pos;
        else
            return -1;
    }

    void UnrefItem(SFrame* vp)
    {
        av_frame_unref(vp->frame);
        avsubtitle_free(&vp->sub);
    }

    SFrame queue[FRAME_QUEUE_SIZE];
    int rindex = 0;
    int windex = 0;
    int size = 0;
    int max_size = 0;
    int keep_last = 0;
    int rindex_shown = 0;
    std::mutex mutex;
    std::condition_variable cond;
    PacketQueue* pktq = nullptr;
};


class Decoder 
{
    friend class FMediaPlayer;
public:
    /* 是否std::shared_ptr 要传引用 */
    void Init(AVCodecContext* avctx, PacketQueue* queue, std::shared_ptr<std::condition_variable> empty_queue_cond) 
    {
        this->avctx = avctx;
        this->queue = queue;
        this->empty_queue_cond = empty_queue_cond;
        this->start_pts = AV_NOPTS_VALUE;
        this->pkt_serial = -1;
    }

    int DecodeFrame(AVFrame* frame, AVSubtitle* sub) 
    {
        int ret = AVERROR(EAGAIN);

        for (;;) {
            AVPacket pkt;

            if (this->queue->serial == this->pkt_serial) 
            {
                do 
                {
                    if (this->queue->abort_request)
                        return -1;

                    switch (this->avctx->codec_type) 
                    {
                    case AVMEDIA_TYPE_VIDEO:
                        ret = avcodec_receive_frame(this->avctx, frame);
                        if (ret >= 0) 
                        {
                            if (decoder_reorder_pts == -1)
                                frame->pts = frame->best_effort_timestamp;
                            
                            else if (!decoder_reorder_pts)
                                frame->pts = frame->pkt_dts;
                        }
                        break;
                    case AVMEDIA_TYPE_AUDIO:
                        ret = avcodec_receive_frame(this->avctx, frame);
                        if (ret >= 0) {
                            AVRational tb = AVRational{ 1, frame->sample_rate };
                            if (frame->pts != AV_NOPTS_VALUE)
                                frame->pts = av_rescale_q(frame->pts, this->avctx->pkt_timebase, tb);
                            else if (this->next_pts != AV_NOPTS_VALUE)
                                frame->pts = av_rescale_q(this->next_pts, this->next_pts_tb, tb);
                            if (frame->pts != AV_NOPTS_VALUE) {
                                this->next_pts = frame->pts + frame->nb_samples;
                                this->next_pts_tb = tb;
                            }
                        }
                        break;
                    }
                    if (ret == AVERROR_EOF) 
                    {
                        this->finished = this->pkt_serial;
                        avcodec_flush_buffers(this->avctx);
                        return 0;
                    }
                    if (ret >= 0)
                        return 1;
                } while (ret != AVERROR(EAGAIN));
            }

            do {
                if (this->queue->nb_packets == 0)
                    this->empty_queue_cond->notify_one();
                if (this->packet_pending) {
                    av_packet_move_ref(&pkt, &this->pkt);
                    this->packet_pending = 0;
                }
                else {
                    if (this->queue->Get(&pkt, 1, &this->pkt_serial) < 0)
                        return -1;
                }
            } while (this->queue->serial != this->pkt_serial);

            if (pkt.data == /*this->queue->_flush_pkt.data*/flush_pkt.data) {
                avcodec_flush_buffers(this->avctx);
                this->finished = 0;
                this->next_pts = this->start_pts;
                this->next_pts_tb = this->start_pts_tb;
            }
            else {
                if (this->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
                    int got_frame = 0;
                    ret = avcodec_decode_subtitle2(this->avctx, sub, &got_frame, &pkt);
                    if (ret < 0) {
                        ret = AVERROR(EAGAIN);
                    }
                    else {
                        if (got_frame && !pkt.data) {
                            this->packet_pending = 1;
                            av_packet_move_ref(&this->pkt, &pkt);
                        }
                        ret = got_frame ? 0 : (pkt.data ? AVERROR(EAGAIN) : AVERROR_EOF);
                    }
                }
                else {
                    if (avcodec_send_packet(this->avctx, &pkt) == AVERROR(EAGAIN)) {
                        av_log(this->avctx, AV_LOG_ERROR, "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
                        this->packet_pending = 1;
                        av_packet_move_ref(&this->pkt, &pkt);
                    }
                }
                av_packet_unref(&pkt);
            }
        }
    }

    int Start(std::function<int(void)>& func)
    {
        this->queue->Start();
        this->future = std::async(std::launch::async, std::move(func));
        if (!this->future.valid())
        {
            av_log(nullptr, AV_LOG_ERROR, "CreateThread Failured!\n");
            return AVERROR(ENOMEM);
        }
        return 0;
    }

    void Destroy() 
    {
        av_packet_unref(&this->pkt);
        avcodec_free_context(&this->avctx);
    }

    void Abort(FrameQueue* fq)
    {
        this->queue->Abort();
        fq->Signal();

        if (future.valid())
        {
            if(future.get() != 0)
                av_log(nullptr, AV_LOG_WARNING, "Thread exit exception\n");
        }

        this->queue->Flush();
    }
private:

    AVPacket pkt;
    PacketQueue* queue = nullptr;
    AVCodecContext* avctx = nullptr;
    int pkt_serial;
    int finished;
    int packet_pending;
    std::shared_ptr<std::condition_variable> empty_queue_cond;
    int64_t start_pts;
    AVRational start_pts_tb;
    int64_t next_pts;
    AVRational next_pts_tb;
    std::future<int> future;
};

class FMediaPlayer 
{
public:
    enum struct EShowMode : Uint8
    {
        SHOW_MODE_NONE = 0, SHOW_MODE_VIDEO, SHOW_MODE_WAVES, SHOW_MODE_RDFT, SHOW_MODE_NB
    };

    enum struct ESyncType : Uint8
    {
        AV_SYNC_AUDIO_MASTER, /* default choice */
        AV_SYNC_VIDEO_MASTER,
        AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
    };

public:
    FMediaPlayer() {};
    ~FMediaPlayer()
    {
        UninitRender();

        stream_close();
    }

    FMediaPlayer(const FMediaPlayer&) = delete;
    FMediaPlayer& operator=(const FMediaPlayer&) = delete;

    static bool InitContext()
    {
        av_log_set_flags(AV_LOG_SKIP_REPEATED);

        /* register all codecs, demux and protocols */
#if CONFIG_AVDEVICE
        avdevice_register_all();
#endif
        avformat_network_init();

        /*-----------------------------------渲染相关设置---------------------------------------*/
        
        int flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
        
        /* Try to work around an occasional ALSA buffer underflow issue when the
         * period size is NPOT due to ALSA resampling by forcing the buffer size. */
        if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE"))
            SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE", "1", 1);
        
        if (SDL_Init(flags)) 
        {
            av_log(nullptr, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
            av_log(nullptr, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
            return false;
        }

        SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
        SDL_EventState(SDL_USEREVENT, SDL_IGNORE);
        /*-----------------------------------渲染相关设置---------------------------------------*/

        return true;
    }

    static void UnInitContext()
    {
        avformat_network_deinit();
    }

    bool InitRender()
    {
       
            int flags = SDL_WINDOW_HIDDEN;
            if (alwaysontop)
#if SDL_VERSION_ATLEAST(2,0,5)
                flags |= SDL_WINDOW_ALWAYS_ON_TOP;
#else
                av_log(nullptr, AV_LOG_WARNING, "Your SDL version doesn't support SDL_WINDOW_ALWAYS_ON_TOP. Feature will be inactive.\n");
#endif
                flags |= SDL_WINDOW_RESIZABLE;
            window = SDL_CreateWindow(program_name, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, default_width, default_height, flags);
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
            if (window) 
            {
                renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
                if (!renderer) {
                    av_log(nullptr, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n", SDL_GetError());
                    renderer = SDL_CreateRenderer(window, -1, 0);
                }
                if (renderer) {
                    if (!SDL_GetRendererInfo(renderer, &renderer_info))
                        av_log(nullptr, AV_LOG_VERBOSE, "Initialized %s renderer.\n", renderer_info.name);
                }
            }
            if (!window || !renderer || !renderer_info.num_texture_formats) {
                av_log(nullptr, AV_LOG_FATAL, "Failed to create window or renderer: %s", SDL_GetError());
                return false;
            }
            return true;
    }

    void UninitRender()
    {
        if (this->vis_texture)
            SDL_DestroyTexture(this->vis_texture);
        if (this->vid_texture)
            SDL_DestroyTexture(this->vid_texture);
        if (this->sub_texture)
            SDL_DestroyTexture(this->sub_texture);

        if (renderer)
            SDL_DestroyRenderer(renderer);
        if (window)
            SDL_DestroyWindow(window);
    }

    void set_default_window_size(int width, int height, AVRational sar)
    {
        SDL_Rect rect;
        int max_width = screen_width ? screen_width : INT_MAX;
        int max_height = screen_height ? screen_height : INT_MAX;
        if (max_width == INT_MAX && max_height == INT_MAX)
            max_height = height;
        calculate_display_rect(&rect, 0, 0, max_width, max_height, width, height, sar);
        default_width = rect.w;
        default_height = rect.h;
    }

    inline void fill_rectangle(int x, int y, int w, int h)
    {
        SDL_Rect rect;
        rect.x = x;
        rect.y = y;
        rect.w = w;
        rect.h = h;
        if (w && h)
            SDL_RenderFillRect(renderer, &rect);
    }

    int realloc_texture(SDL_Texture** texture, Uint32 new_format, int new_width, int new_height, SDL_BlendMode blendmode, int init_texture)
    {
        Uint32 format;
        int access, w, h;
        if (!*texture || SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 || new_width != w || new_height != h || new_format != format) {
            void* pixels;
            int pitch;
            if (*texture)
                SDL_DestroyTexture(*texture);
            if (!(*texture = SDL_CreateTexture(renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width, new_height)))
                return -1;
            if (SDL_SetTextureBlendMode(*texture, blendmode) < 0)
                return -1;
            if (init_texture) {
                if (SDL_LockTexture(*texture, nullptr, &pixels, &pitch) < 0)
                    return -1;
                memset(pixels, 0, pitch * new_height);
                SDL_UnlockTexture(*texture);
            }
            av_log(nullptr, AV_LOG_VERBOSE, "Created %dx%d texture with %s.\n", new_width, new_height, SDL_GetPixelFormatName(new_format));
        }
        return 0;
    }

    void calculate_display_rect(SDL_Rect* rect,
        int scr_xleft, int scr_ytop, int scr_width, int scr_height,
        int pic_width, int pic_height, AVRational pic_sar)
    {
        AVRational aspect_ratio = pic_sar;
        int64_t width, height, x, y;

        if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0)
            aspect_ratio = av_make_q(1, 1);

        aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

        /* XXX: we suppose the screen has a 1.0 pixel ratio */
        height = scr_height;
        width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
        if (width > scr_width) {
            width = scr_width;
            height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
        }
        x = (scr_width - width) / 2;
        y = (scr_height - height) / 2;
        rect->x = scr_xleft + x;
        rect->y = scr_ytop + y;
        rect->w = FFMAX((int)width, 1);
        rect->h = FFMAX((int)height, 1);
    }

    void get_sdl_pix_fmt_and_blendmode(int format, Uint32* sdl_pix_fmt, SDL_BlendMode* sdl_blendmode)
    {
        int i;
        *sdl_blendmode = SDL_BLENDMODE_NONE;
        *sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;
        if (format == AV_PIX_FMT_RGB32 ||
            format == AV_PIX_FMT_RGB32_1 ||
            format == AV_PIX_FMT_BGR32 ||
            format == AV_PIX_FMT_BGR32_1)
            *sdl_blendmode = SDL_BLENDMODE_BLEND;
        for (i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; i++) {
            if (format == sdl_texture_format_map[i].format) {
                *sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
                return;
            }
        }
    }

    int upload_texture(SDL_Texture** tex, AVFrame* frame, struct SwsContext** img_convert_ctx) 
    {
        int ret = 0;
        Uint32 sdl_pix_fmt;
        SDL_BlendMode sdl_blendmode;
        get_sdl_pix_fmt_and_blendmode(frame->format, &sdl_pix_fmt, &sdl_blendmode);
        if (realloc_texture(tex, sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN ? SDL_PIXELFORMAT_ARGB8888 : sdl_pix_fmt, frame->width, frame->height, sdl_blendmode, 0) < 0)
            return -1;
        switch (sdl_pix_fmt) {
        case SDL_PIXELFORMAT_UNKNOWN:
            /* This should only happen if we are not using avfilter... */
            *img_convert_ctx = sws_getCachedContext(*img_convert_ctx,
                frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), frame->width, frame->height,
                AV_PIX_FMT_BGRA, sws_flags, nullptr, nullptr, nullptr);
            if (*img_convert_ctx != nullptr) {
                uint8_t* pixels[4];
                int pitch[4];
                if (!SDL_LockTexture(*tex, nullptr, (void**)pixels, pitch)) {
                    sws_scale(*img_convert_ctx, (const uint8_t* const*)frame->data, frame->linesize,
                        0, frame->height, pixels, pitch);
                    SDL_UnlockTexture(*tex);
                }
            }
            else {
                av_log(nullptr, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
                ret = -1;
            }
            break;
        case SDL_PIXELFORMAT_IYUV:
            if (frame->linesize[0] > 0 && frame->linesize[1] > 0 && frame->linesize[2] > 0) {
                ret = SDL_UpdateYUVTexture(*tex, nullptr, frame->data[0], frame->linesize[0],
                    frame->data[1], frame->linesize[1],
                    frame->data[2], frame->linesize[2]);
            }
            else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 && frame->linesize[2] < 0) {
                ret = SDL_UpdateYUVTexture(*tex, nullptr, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0],
                    frame->data[1] + frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[1],
                    frame->data[2] + frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[2]);
            }
            else {
                av_log(nullptr, AV_LOG_ERROR, "Mixed negative and positive linesizes are not supported.\n");
                return -1;
            }
            break;
        default:
            if (frame->linesize[0] < 0) {
                ret = SDL_UpdateTexture(*tex, nullptr, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0]);
            }
            else {
                ret = SDL_UpdateTexture(*tex, nullptr, frame->data[0], frame->linesize[0]);
            }
            break;
        }
        return ret;
    }

    void set_sdl_yuv_conversion_mode(AVFrame* frame)
    {
#if SDL_VERSION_ATLEAST(2,0,8)
        SDL_YUV_CONVERSION_MODE mode = SDL_YUV_CONVERSION_AUTOMATIC;
        if (frame && (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUYV422 || frame->format == AV_PIX_FMT_UYVY422)) {
            if (frame->color_range == AVCOL_RANGE_JPEG)
                mode = SDL_YUV_CONVERSION_JPEG;
            else if (frame->colorspace == AVCOL_SPC_BT709)
                mode = SDL_YUV_CONVERSION_BT709;
            else if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M || frame->colorspace == AVCOL_SPC_SMPTE240M)
                mode = SDL_YUV_CONVERSION_BT601;
        }
        SDL_SetYUVConversionMode(mode);
#endif
    }

    void video_image_display()
    {
        SFrame* vp;
        SFrame* sp = nullptr;
        SDL_Rect rect;

        vp = this->pictq.PeekLast();
        if (this->subtitle_st) {
            if (this->subpq.NbRemaining() > 0) {
                sp = this->subpq.Peek();

                if (vp->pts >= sp->pts + ((float)sp->sub.start_display_time / 1000)) {
                    if (!sp->uploaded) {
                        uint8_t* pixels[4];
                        int pitch[4];
                        int i;
                        if (!sp->width || !sp->height) {
                            sp->width = vp->width;
                            sp->height = vp->height;
                        }
                        if (realloc_texture(&this->sub_texture, SDL_PIXELFORMAT_ARGB8888, sp->width, sp->height, SDL_BLENDMODE_BLEND, 1) < 0)
                            return;

                        for (i = 0; i < sp->sub.num_rects; i++) {
                            AVSubtitleRect* sub_rect = sp->sub.rects[i];

                            sub_rect->x = av_clip(sub_rect->x, 0, sp->width);
                            sub_rect->y = av_clip(sub_rect->y, 0, sp->height);
                            sub_rect->w = av_clip(sub_rect->w, 0, sp->width - sub_rect->x);
                            sub_rect->h = av_clip(sub_rect->h, 0, sp->height - sub_rect->y);

                            this->sub_convert_ctx = sws_getCachedContext(this->sub_convert_ctx,
                                sub_rect->w, sub_rect->h, AV_PIX_FMT_PAL8,
                                sub_rect->w, sub_rect->h, AV_PIX_FMT_BGRA,
                                0, nullptr, nullptr, nullptr);
                            if (!this->sub_convert_ctx) {
                                av_log(nullptr, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
                                return;
                            }
                            if (!SDL_LockTexture(this->sub_texture, (SDL_Rect*)sub_rect, (void**)pixels, pitch)) {
                                sws_scale(this->sub_convert_ctx, (const uint8_t* const*)sub_rect->data, sub_rect->linesize,
                                    0, sub_rect->h, pixels, pitch);
                                SDL_UnlockTexture(this->sub_texture);
                            }
                        }
                        sp->uploaded = 1;
                    }
                }
                else
                    sp = nullptr;
            }
        }

        calculate_display_rect(&rect, this->xleft, this->ytop, this->width, this->height, vp->width, vp->height, vp->sar);

        if (!vp->uploaded) {
            if (upload_texture(&this->vid_texture, vp->frame, &this->img_convert_ctx) < 0)
                return;
            vp->uploaded = 1;
            vp->flip_v = vp->frame->linesize[0] < 0;
        }

        set_sdl_yuv_conversion_mode(vp->frame);
        SDL_RenderCopyEx(renderer, this->vid_texture, nullptr, &rect, 0, nullptr, static_cast<SDL_RendererFlip>(vp->flip_v ? SDL_FLIP_VERTICAL : 0));
        set_sdl_yuv_conversion_mode(nullptr);
        if (sp) {
#if USE_ONEPASS_SUBTITLE_RENDER
            SDL_RenderCopy(renderer, this->sub_texture, nullptr, &rect);
#else
            int i;
            double xratio = (double)rect.w / (double)sp->width;
            double yratio = (double)rect.h / (double)sp->height;
            for (i = 0; i < sp->sub.num_rects; i++) {
                SDL_Rect* sub_rect = (SDL_Rect*)sp->sub.rects[i];
                SDL_Rect target;
                target.x = rect.x + sub_rect->x * xratio;
                target.y = rect.y + sub_rect->y * yratio;
                target.w = sub_rect->w * xratio;
                target.h = sub_rect->h * yratio;
                SDL_RenderCopy(renderer, this->sub_texture, sub_rect, &target);
            }
#endif
        }
    }

    static inline int compute_mod(int a, int b)
    {
        return a < 0 ? a % b + b : a % b;
    }

    void video_audio_display()
    {
        int i, i_start, x, y1, y, ys, delay, n, nb_display_channels;
        int ch, channels, h, h2;
        int64_t time_diff;
        int rdft_bits, nb_freq;

        for (rdft_bits = 1; (1 << rdft_bits) < 2 * this->height; rdft_bits++)
            ;
        nb_freq = 1 << (rdft_bits - 1);

        /* compute display index : center on currently output samples */
        channels = this->audio_tgt.channels;
        nb_display_channels = channels;
        if (!this->paused) {
            int data_used = this->eShow_mode == FMediaPlayer::EShowMode::SHOW_MODE_WAVES ? this->width : (2 * nb_freq);
            n = 2 * channels;
            delay = this->audio_write_buf_size;
            delay /= n;

            /* to be more precise, we take into account the time spent since
               the last buffer computation */
            if (audio_callback_time) {
                time_diff = av_gettime_relative() - audio_callback_time;
                delay -= (time_diff * this->audio_tgt.freq) / 1000000;
            }

            delay += 2 * data_used;
            if (delay < data_used)
                delay = data_used;

            i_start = x = compute_mod(this->sample_array_index - delay * channels, SAMPLE_ARRAY_SIZE);
            if (this->eShow_mode == FMediaPlayer::EShowMode::SHOW_MODE_WAVES) {
                h = INT_MIN;
                for (i = 0; i < 1000; i += channels) {
                    int idx = (SAMPLE_ARRAY_SIZE + x - i) % SAMPLE_ARRAY_SIZE;
                    int a = this->sample_array[idx];
                    int b = this->sample_array[(idx + 4 * channels) % SAMPLE_ARRAY_SIZE];
                    int c = this->sample_array[(idx + 5 * channels) % SAMPLE_ARRAY_SIZE];
                    int d = this->sample_array[(idx + 9 * channels) % SAMPLE_ARRAY_SIZE];
                    int score = a - d;
                    if (h < score && (b ^ c) < 0) {
                        h = score;
                        i_start = idx;
                    }
                }
            }

            this->last_i_start = i_start;
        }
        else {
            i_start = this->last_i_start;
        }

        if (this->eShow_mode == FMediaPlayer::EShowMode::SHOW_MODE_WAVES) {
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);

            /* total height for one channel */
            h = this->height / nb_display_channels;
            /* graph height / 2 */
            h2 = (h * 9) / 20;
            for (ch = 0; ch < nb_display_channels; ch++) {
                i = i_start + ch;
                y1 = this->ytop + ch * h + (h / 2); /* position of center line */
                for (x = 0; x < this->width; x++) {
                    y = (this->sample_array[i] * h2) >> 15;
                    if (y < 0) {
                        y = -y;
                        ys = y1 - y;
                    }
                    else {
                        ys = y1;
                    }
                    fill_rectangle(this->xleft + x, ys, 1, y);
                    i += channels;
                    if (i >= SAMPLE_ARRAY_SIZE)
                        i -= SAMPLE_ARRAY_SIZE;
                }
            }

            SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);

            for (ch = 1; ch < nb_display_channels; ch++) {
                y = this->ytop + ch * h;
                fill_rectangle(this->xleft, y, this->width, 1);
            }
        }
        else {
            if (realloc_texture(&this->vis_texture, SDL_PIXELFORMAT_ARGB8888, this->width, this->height, SDL_BLENDMODE_NONE, 1) < 0)
                return;

            nb_display_channels = FFMIN(nb_display_channels, 2);
            if (rdft_bits != this->rdft_bits) {
                av_rdft_end(this->rdft);
                av_free(this->rdft_data);
                this->rdft = av_rdft_init(rdft_bits, DFT_R2C);
                this->rdft_bits = rdft_bits;
                this->rdft_data = static_cast<FFTSample*>(av_malloc_array(nb_freq, 4 * sizeof(*this->rdft_data)));
            }
            if (!this->rdft || !this->rdft_data) {
                av_log(nullptr, AV_LOG_ERROR, "Failed to allocate buffers for RDFT, switching to waves display\n");
                this->eShow_mode = FMediaPlayer::EShowMode::SHOW_MODE_WAVES;
            }
            else {
                FFTSample* data[2];
                SDL_Rect rect;
                rect.x = this->xpos; rect.y = 0; rect.w = 1; rect.h = this->height;

                uint32_t* pixels;
                int pitch;
                for (ch = 0; ch < nb_display_channels; ch++) {
                    data[ch] = this->rdft_data + 2 * nb_freq * ch;
                    i = i_start + ch;
                    for (x = 0; x < 2 * nb_freq; x++) {
                        double w = (x - nb_freq) * (1.0 / nb_freq);
                        data[ch][x] = this->sample_array[i] * (1.0 - w * w);
                        i += channels;
                        if (i >= SAMPLE_ARRAY_SIZE)
                            i -= SAMPLE_ARRAY_SIZE;
                    }
                    av_rdft_calc(this->rdft, data[ch]);
                }
                /* Least efficient way to do this, we should of course
                 * directly access it but it is more than fast enough. */
                if (!SDL_LockTexture(this->vis_texture, &rect, (void**)&pixels, &pitch)) {
                    pitch >>= 2;
                    pixels += pitch * this->height;
                    for (y = 0; y < this->height; y++) {
                        double w = 1 / sqrt(nb_freq);
                        int a = sqrt(w * sqrt(data[0][2 * y + 0] * data[0][2 * y + 0] + data[0][2 * y + 1] * data[0][2 * y + 1]));
                        int b = (nb_display_channels == 2) ? sqrt(w * hypot(data[1][2 * y + 0], data[1][2 * y + 1]))
                            : a;
                        a = FFMIN(a, 255);
                        b = FFMIN(b, 255);
                        pixels -= pitch;
                        *pixels = (a << 16) + (b << 8) + ((a + b) >> 1);
                    }
                    SDL_UnlockTexture(this->vis_texture);
                }
                SDL_RenderCopy(renderer, this->vis_texture, nullptr, nullptr);
            }
            if (!this->paused)
                this->xpos++;
            if (this->xpos >= this->width)
                this->xpos = this->xleft;
        }
    }

    void stream_component_close(int stream_index)
    {
        AVFormatContext* ic = this->ic;
        AVCodecParameters* codecpar;

        if (stream_index < 0 || stream_index >= ic->nb_streams)
            return;
        codecpar = ic->streams[stream_index]->codecpar;

        switch (codecpar->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            this->auddec.Abort(&this->sampq);
            SDL_CloseAudioDevice(audio_dev);
            this->auddec.Destroy();
            swr_free(&this->swr_ctx);
            av_freep(&this->audio_buf1);
            this->audio_buf1_size = 0;
            this->audio_buf = nullptr;

            if (this->rdft) {
                av_rdft_end(this->rdft);
                av_freep(&this->rdft_data);
                this->rdft = nullptr;
                this->rdft_bits = 0;
            }
            break;
        case AVMEDIA_TYPE_VIDEO:
            this->viddec.Abort(&this->pictq);
            this->viddec.Destroy();
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            this->subdec.Abort(&this->subpq);
            this->subdec.Destroy();
            break;
        default:
            break;
        }

        ic->streams[stream_index]->discard = AVDISCARD_ALL;
        switch (codecpar->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            this->audio_st = nullptr;
            this->audio_stream = -1;
            break;
        case AVMEDIA_TYPE_VIDEO:
            this->video_st = nullptr;
            this->video_stream = -1;
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            this->subtitle_st = nullptr;
            this->subtitle_stream = -1;
            break;
        default:
            break;
        }
    }

    void stream_close()
    {
        /* XXX: use a special url_shutdown call to abort parse cleanly */
        this->abort_request = 1;
        if (this->future.valid())
            if(this->future.get() != 0)
                av_log(nullptr, AV_LOG_WARNING, "Thread exit exception\n");

        /* close each stream */
        if (this->audio_stream >= 0)
            stream_component_close(this->audio_stream);
        if (this->video_stream >= 0)
            stream_component_close(this->video_stream);
        if (this->subtitle_stream >= 0)
            stream_component_close(this->subtitle_stream);

        avformat_close_input(&this->ic);

        this->videoq.Destroy();
        this->audioq.Destroy();
        this->subtitleq.Destroy();

        /* free all pictures */
        this->pictq.Destory();
        this->sampq.Destory();
        this->subpq.Destory();
        //SDL_DestroyCond(this->continue_read_thread);
        sws_freeContext(this->img_convert_ctx);
        sws_freeContext(this->sub_convert_ctx);
        av_free(this->filename);
    }

    void do_exit()
    {
        stream_close();

        UninitRender();

        uninit_opts();
#if CONFIG_AVFILTER
        av_freep(&vfilters_list);
#endif
        
        av_log(nullptr, AV_LOG_QUIET, "%s", "");

        

        exit(0);
    }

    static void sigterm_handler(int sig)
    {
        exit(123);
    }

    int video_open()
    {
        int w, h;

        w = screen_width ? screen_width : default_width;
        h = screen_height ? screen_height : default_height;

        SDL_SetWindowSize(window, w, h);
        SDL_SetWindowPosition(window, screen_left, screen_top);
        if (is_full_screen)
            SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
        SDL_ShowWindow(window);

        this->width = w;
        this->height = h;

        return 0;
    }

    /* display the current picture, if any */
    void video_display()
    {
        try
        {
            if (!this->width)
                video_open();

            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
            if (this->audio_st && this->eShow_mode != FMediaPlayer::EShowMode::SHOW_MODE_VIDEO)
                video_audio_display();
            else if (this->video_st)
                video_image_display();
            SDL_RenderPresent(renderer);
        }
        catch (...)
        {
            av_log(nullptr, AV_LOG_ERROR, "Error::video_display\n");
            return;
        }
    }

    static void sync_clock_to_slave(SClock* c, SClock* slave)
    {
        double clock = c->Get();
        double slave_clock = slave->Get();
        if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
            c->Set(slave_clock, slave->serial);
    }

    ESyncType get_master_sync_type()
    {
        if (this->av_sync_type == ESyncType::AV_SYNC_VIDEO_MASTER)
        {
            if (this->video_st)
                return ESyncType::AV_SYNC_VIDEO_MASTER;
            else
                return ESyncType::AV_SYNC_AUDIO_MASTER;
        }
        else if (this->av_sync_type == ESyncType::AV_SYNC_AUDIO_MASTER)
        {
            if (this->audio_st)
                return ESyncType::AV_SYNC_AUDIO_MASTER;
            else
                return ESyncType::AV_SYNC_EXTERNAL_CLOCK;
        }
        else {
            return ESyncType::AV_SYNC_EXTERNAL_CLOCK;
        }
    }

    /* get the current master clock value */
    double get_master_clock()
    {
        double val;

        switch (get_master_sync_type()) {
        case ESyncType::AV_SYNC_VIDEO_MASTER:
            val = this->vidclk.Get();
            break;
        case ESyncType::AV_SYNC_AUDIO_MASTER:
            val = this->audclk.Get();
            break;
        default:
            val = this->extclk.Get();
            break;
        }
        return val;
    }

    void check_external_clock_speed() 
    {
        if (this->video_stream >= 0 && this->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES ||
            this->audio_stream >= 0 && this->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES) {
            this->extclk.SetSpeed(FFMAX(EXTERNAL_CLOCK_SPEED_MIN, this->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
        }
        else if ((this->video_stream < 0 || this->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) &&
            (this->audio_stream < 0 || this->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)) {
            this->extclk.SetSpeed(FFMIN(EXTERNAL_CLOCK_SPEED_MAX, this->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
        }
        else {
            double speed = this->extclk.speed;
            if (speed != 1.0)
                this->extclk.SetSpeed(speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
        }
    }

    /* seek in the stream */
    void stream_seek(int64_t pos, int64_t rel, int seek_by_bytes)
    {
        if (!this->seek_req) 
        {
            this->seek_pos = pos;
            this->seek_rel = rel;
            this->seek_flags &= ~AVSEEK_FLAG_BYTE;
            if (seek_by_bytes)
                this->seek_flags |= AVSEEK_FLAG_BYTE;
            this->seek_req = 1;
            this->continue_read_thread->notify_one();
        }
    }

    /* pause or resume the video */
    void stream_toggle_pause()
    {
        if (this->paused) 
        {
            this->frame_timer += av_gettime_relative() / 1000000.0 - this->vidclk.last_updated;
            if (this->read_pause_return != AVERROR(ENOSYS))
                this->vidclk.paused = 0;
            
            this->vidclk.Set(this->vidclk.Get(), this->vidclk.serial);
        }
        this->extclk.Set(this->extclk.Get(), this->extclk.serial);
        this->paused = this->audclk.paused = this->vidclk.paused = this->extclk.paused = !this->paused;
    }

    void toggle_pause()
    {
        stream_toggle_pause();
        this->step = 0;
    }

    void toggle_mute()
    {
        this->muted = !this->muted;
    }

    void update_volume(int sign, double step)
    {
        double volume_level = this->audio_volume ? (20 * log(this->audio_volume / (double)SDL_MIX_MAXVOLUME) / log(10)) : -1000.0;
        int new_volume = lrint(SDL_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0));
        this->audio_volume = av_clip(this->audio_volume == new_volume ? (this->audio_volume + sign) : new_volume, 0, SDL_MIX_MAXVOLUME);
    }

    void step_to_next_frame()
    {
        /* if the stream is paused unpause it, then step */
        if (this->paused)
            stream_toggle_pause();
        this->step = 1;
    }

    double compute_target_delay(double delay)
    {
        double sync_threshold, diff = 0;

        /* update delay to follow master synchronisation source */
        if (get_master_sync_type() != ESyncType::AV_SYNC_VIDEO_MASTER) {
            /* if video is slave, we try to correct big delays by
               duplicating or deleting a frame */
            diff = this->vidclk.Get() - get_master_clock();

            /* skip or repeat frame. We take into account the
               delay to compute the threshold. I still don't know
               if it is the best guess */
            sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
            if (!isnan(diff) && fabs(diff) < this->max_frame_duration) {
                if (diff <= -sync_threshold)
                    delay = FFMAX(0, delay + diff);
                else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
                    delay = delay + diff;
                else if (diff >= sync_threshold)
                    delay = 2 * delay;
            }
        }

        av_log(nullptr, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n",
            delay, -diff);

        return delay;
    }

    double vp_duration(SFrame* vp, SFrame* nextvp) 
    {
        if (vp->serial == nextvp->serial) {
            double duration = nextvp->pts - vp->pts;
            if (isnan(duration) || duration <= 0 || duration > this->max_frame_duration)
                return vp->duration;
            else
                return duration;
        }
        else {
            return 0.0;
        }
    }

    void update_video_pts(double pts, int64_t pos, int serial) 
    {
        /* update current video pts */
        this->vidclk.Set(pts, serial);
        sync_clock_to_slave(&this->extclk, &this->vidclk);
    }

    /* called to display each frame */
    void video_refresh(double* remaining_time)
    {
        double time;

        SFrame* sp = nullptr, * sp2 = nullptr;

        if (!this->paused && get_master_sync_type() == FMediaPlayer::ESyncType::AV_SYNC_EXTERNAL_CLOCK && this->realtime)
            check_external_clock_speed();

        if (this->eShow_mode != FMediaPlayer::EShowMode::SHOW_MODE_VIDEO && this->audio_st) 
        {
            time = av_gettime_relative() / 1000000.0;
            if (this->force_refresh || this->last_vis_time + rdftspeed < time) {
                video_display();
                this->last_vis_time = time;
            }
            *remaining_time = FFMIN(*remaining_time, this->last_vis_time + rdftspeed - time);
        }

        if (this->video_st) {
        retry:
            if (this->pictq.NbRemaining() == 0) {
                // nothing to do, no picture to display in the queue
            }
            else {
                double last_duration, duration, delay;
                SFrame* vp, * lastvp;

                /* dequeue the picture */
                lastvp = this->pictq.PeekLast();
                vp = this->pictq.Peek();

                if (vp->serial != this->videoq.serial) {
                    this->pictq.Next();
                    goto retry;
                }

                if (lastvp->serial != vp->serial)
                    this->frame_timer = av_gettime_relative() / 1000000.0;

                if (this->paused)
                    goto display;

                /* compute nominal last_duration */
                last_duration = vp_duration(lastvp, vp);
                delay = compute_target_delay(last_duration);

                time = av_gettime_relative() / 1000000.0;
                if (time < this->frame_timer + delay) {
                    *remaining_time = FFMIN(this->frame_timer + delay - time, *remaining_time);
                    goto display;
                }

                this->frame_timer += delay;
                if (delay > 0 && time - this->frame_timer > AV_SYNC_THRESHOLD_MAX)
                    this->frame_timer = time;

                {
                    std::lock_guard<std::mutex> lock(this->pictq.mutex);
                    if (!isnan(vp->pts))
                        update_video_pts(vp->pts, vp->pos, vp->serial);
                }

                if (this->pictq.NbRemaining() > 1) {
                    SFrame* nextvp = this->pictq.PeekNext();
                    duration = vp_duration(vp, nextvp);
                    if (!this->step && (framedrop > 0 || (framedrop && get_master_sync_type() != FMediaPlayer::ESyncType::AV_SYNC_VIDEO_MASTER)) && time > this->frame_timer + duration) {
                        this->frame_drops_late++;
                        this->pictq.Next();
                        goto retry;
                    }
                }

                if (this->subtitle_st) {
                    while (this->subpq.NbRemaining() > 0) {
                        sp = this->subpq.Peek();

                        if (this->subpq.NbRemaining() > 1)
                            sp2 = this->subpq.PeekNext();
                        else
                            sp2 = nullptr;

                        if (sp->serial != this->subtitleq.serial
                            || (this->vidclk.pts > (sp->pts + ((float)sp->sub.end_display_time / 1000)))
                            || (sp2 && this->vidclk.pts > (sp2->pts + ((float)sp2->sub.start_display_time / 1000))))
                        {
                            if (sp->uploaded) {
                                int i;
                                for (i = 0; i < sp->sub.num_rects; i++) {
                                    AVSubtitleRect* sub_rect = sp->sub.rects[i];
                                    uint8_t* pixels;
                                    int pitch, j;

                                    if (!SDL_LockTexture(this->sub_texture, (SDL_Rect*)sub_rect, (void**)&pixels, &pitch)) {
                                        for (j = 0; j < sub_rect->h; j++, pixels += pitch)
                                            memset(pixels, 0, sub_rect->w << 2);
                                        SDL_UnlockTexture(this->sub_texture);
                                    }
                                }
                            }
                            this->subpq.Next();
                        }
                        else {
                            break;
                        }
                    }
                }

                this->pictq.Next();
                this->force_refresh = 1;

                if (this->step && !this->paused)
                    stream_toggle_pause();
            }
        display:
            /* display picture */
            if (this->force_refresh && this->eShow_mode == FMediaPlayer::EShowMode::SHOW_MODE_VIDEO && this->pictq.rindex_shown)
                video_display();
        }
        this->force_refresh = 0;
        if (show_status) {
            static int64_t last_time;
            int64_t cur_time;
            int aqsize, vqsize, sqsize;
            double av_diff;

            cur_time = av_gettime_relative();
            if (!last_time || (cur_time - last_time) >= 30000) 
            {
                aqsize = 0;
                vqsize = 0;
                sqsize = 0;
                if (this->audio_st)
                    aqsize = this->audioq.size;
                if (this->video_st)
                    vqsize = this->videoq.size;
                if (this->subtitle_st)
                    sqsize = this->subtitleq.size;
                av_diff = 0;
                if (this->audio_st && this->video_st)
                    av_diff = this->audclk.Get() - this->vidclk.Get();
                else if (this->video_st)
                    av_diff = get_master_clock() - this->vidclk.Get();
                else if (this->audio_st)
                    av_diff = get_master_clock() - this->audclk.Get();
                /*av_log(nullptr, AV_LOG_INFO,
                    "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB f=%"PRId64"/%"PRId64"   \r",
                    get_master_clock(is),
                    (pPlayer->audio_st && pPlayer->video_st) ? "A-V" : (pPlayer->video_st ? "M-V" : (pPlayer->audio_st ? "M-A" : "   ")),
                    av_diff,
                    pPlayer->frame_drops_early + pPlayer->frame_drops_late,
                    aqsize / 1024,
                    vqsize / 1024,
                    sqsize,
                    pPlayer->video_st ? pPlayer->viddec.avctx->pts_correction_num_faulty_dts : 0,
                    pPlayer->video_st ? pPlayer->viddec.avctx->pts_correction_num_faulty_pts : 0);*/
                fflush(stdout);
                last_time = cur_time;
            }
        }
    }

    int queue_picture(AVFrame* src_frame, double pts, double duration, int64_t pos, int serial)
    {
        SFrame* vp;

#if defined(DEBUG_SYNC)
        printf("frame_type=%c pts=%0.3f\n",
            av_get_picture_type_char(src_frame->pict_type), pts);
#endif

        if (!(vp = this->pictq.PeekWritable()))
            return -1;

        vp->sar = src_frame->sample_aspect_ratio;
        vp->uploaded = 0;

        vp->width = src_frame->width;
        vp->height = src_frame->height;
        vp->format = src_frame->format;

        vp->pts = pts;
        vp->duration = duration;
        vp->pos = pos;
        vp->serial = serial;

        set_default_window_size(vp->width, vp->height, vp->sar);

        av_frame_move_ref(vp->frame, src_frame);
        this->pictq.Push();
        return 0;
    }

    int get_video_frame(AVFrame* frame)
    {
        int got_picture;

        if ((got_picture = this->viddec.DecodeFrame(frame, nullptr)) < 0)
            return -1;

        if (got_picture) {
            double dpts = NAN;

            if (frame->pts != AV_NOPTS_VALUE)
                dpts = av_q2d(this->video_st->time_base) * frame->pts;

            frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(this->ic, this->video_st, frame);

            if (framedrop > 0 || (framedrop && get_master_sync_type() != ESyncType::AV_SYNC_VIDEO_MASTER)) {
                if (frame->pts != AV_NOPTS_VALUE) {
                    double diff = dpts - get_master_clock();
                    if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                        diff - this->frame_last_filter_delay < 0 &&
                        this->viddec.pkt_serial == this->vidclk.serial &&
                        this->videoq.nb_packets) {
                        this->frame_drops_early++;
                        av_frame_unref(frame);
                        got_picture = 0;
                    }
                }
            }
        }

        return got_picture;
    }

#if CONFIG_AVFILTER
    static int configure_filtergraph(AVFilterGraph* graph, const char* filtergraph,
        AVFilterContext* source_ctx, AVFilterContext* sink_ctx)
    {
        int ret, i;
        int nb_filters = graph->nb_filters;
        AVFilterInOut* outputs = nullptr, * inputs = nullptr;

        if (filtergraph) {
            outputs = avfilter_inout_alloc();
            inputs = avfilter_inout_alloc();
            if (!outputs || !inputs) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }

            outputs->name = av_strdup("in");
            outputs->filter_ctx = source_ctx;
            outputs->pad_idx = 0;
            outputs->next = nullptr;

            inputs->name = av_strdup("out");
            inputs->filter_ctx = sink_ctx;
            inputs->pad_idx = 0;
            inputs->next = nullptr;

            if ((ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs, nullptr)) < 0)
                goto fail;
        }
        else {
            if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0)
                goto fail;
        }

        /* Reorder the filters to ensure that inputs of the custom filters are merged first */
        for (i = 0; i < graph->nb_filters - nb_filters; i++)
            FFSWAP(AVFilterContext*, graph->filters[i], graph->filters[i + nb_filters]);

        ret = avfilter_graph_config(graph, nullptr);
    fail:
        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);
        return ret;
    }

    static int configure_video_filters(AVFilterGraph* graph, VideoState* is, const char* vfilters, AVFrame* frame)
    {
        enum AVPixelFormat pix_fmts[FF_ARRAY_ELEMS(sdl_texture_format_map)];
        char sws_flags_str[512] = "";
        char buffersrc_args[256];
        int ret;
        AVFilterContext* filt_src = nullptr, * filt_out = nullptr, * last_filter = nullptr;
        AVCodecParameters* codecpar = is->video_st->codecpar;
        AVRational fr = av_guess_frame_rate(is->ic, is->video_st, nullptr);
        AVDictionaryEntry* e = nullptr;
        int nb_pix_fmts = 0;
        int i, j;

        for (i = 0; i < renderer_info.num_texture_formats; i++) {
            for (j = 0; j < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; j++) {
                if (renderer_info.texture_formats[i] == sdl_texture_format_map[j].texture_fmt) {
                    pix_fmts[nb_pix_fmts++] = sdl_texture_format_map[j].format;
                    break;
                }
            }
        }
        pix_fmts[nb_pix_fmts] = AV_PIX_FMT_NONE;

        while ((e = av_dict_get(sws_dict, "", e, AV_DICT_IGNORE_SUFFIX))) {
            if (!strcmp(e->key, "sws_flags")) {
                av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags", e->value);
            }
            else
                av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", e->key, e->value);
        }
        if (strlen(sws_flags_str))
            sws_flags_str[strlen(sws_flags_str) - 1] = '\0';

        graph->scale_sws_opts = av_strdup(sws_flags_str);

        snprintf(buffersrc_args, sizeof(buffersrc_args),
            "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
            frame->width, frame->height, frame->format,
            is->video_st->time_base.num, is->video_st->time_base.den,
            codecpar->sample_aspect_ratio.num, FFMAX(codecpar->sample_aspect_ratio.den, 1));
        if (fr.num && fr.den)
            av_strlcatf(buffersrc_args, sizeof(buffersrc_args), ":frame_rate=%d/%d", fr.num, fr.den);

        if ((ret = avfilter_graph_create_filter(&filt_src,
            avfilter_get_by_name("buffer"),
            "ffplay_buffer", buffersrc_args, nullptr,
            graph)) < 0)
            goto fail;

        ret = avfilter_graph_create_filter(&filt_out,
            avfilter_get_by_name("buffersink"),
            "ffplay_buffersink", nullptr, nullptr, graph);
        if (ret < 0)
            goto fail;

        if ((ret = av_opt_set_int_list(filt_out, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto fail;

        last_filter = filt_out;

        /* Note: this macro adds a filter before the lastly added filter, so the
         * processing order of the filters is in reverse */
#define INSERT_FILT(name, arg) do {                                          \
    AVFilterContext *filt_ctx;                                               \
                                                                             \
    ret = avfilter_graph_create_filter(&filt_ctx,                            \
                                       avfilter_get_by_name(name),           \
                                       "ffplay_" name, arg, nullptr, graph);    \
    if (ret < 0)                                                             \
        goto fail;                                                           \
                                                                             \
    ret = avfilter_link(filt_ctx, 0, last_filter, 0);                        \
    if (ret < 0)                                                             \
        goto fail;                                                           \
                                                                             \
    last_filter = filt_ctx;                                                  \
} while (0)

        if (autorotate) {
            double theta = get_rotation(is->video_st);

            if (fabs(theta - 90) < 1.0) {
                INSERT_FILT("transpose", "clock");
            }
            else if (fabs(theta - 180) < 1.0) {
                INSERT_FILT("hflip", nullptr);
                INSERT_FILT("vflip", nullptr);
            }
            else if (fabs(theta - 270) < 1.0) {
                INSERT_FILT("transpose", "cclock");
            }
            else if (fabs(theta) > 1.0) {
                char rotate_buf[64];
                snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
                INSERT_FILT("rotate", rotate_buf);
            }
        }

        if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0)
            goto fail;

        is->in_video_filter = filt_src;
        is->out_video_filter = filt_out;

    fail:
        return ret;
    }

    static int configure_audio_filters(VideoState* is, const char* afilters, int force_output_format)
    {
        static const enum AVSampleFormat sample_fmts[] = { AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE };
        int sample_rates[2] = { 0, -1 };
        int64_t channel_layouts[2] = { 0, -1 };
        int channels[2] = { 0, -1 };
        AVFilterContext* filt_asrc = nullptr, * filt_asink = nullptr;
        char aresample_swr_opts[512] = "";
        AVDictionaryEntry* e = nullptr;
        char asrc_args[256];
        int ret;

        avfilter_graph_free(&is->agraph);
        if (!(is->agraph = avfilter_graph_alloc()))
            return AVERROR(ENOMEM);
        is->agraph->nb_threads = filter_nbthreads;

        while ((e = av_dict_get(swr_opts, "", e, AV_DICT_IGNORE_SUFFIX)))
            av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);
        if (strlen(aresample_swr_opts))
            aresample_swr_opts[strlen(aresample_swr_opts) - 1] = '\0';
        av_opt_set(is->agraph, "aresample_swr_opts", aresample_swr_opts, 0);

        ret = snprintf(asrc_args, sizeof(asrc_args),
            "sample_rate=%d:sample_fmt=%s:channels=%d:time_base=%d/%d",
            is->audio_filter_src.freq, av_get_sample_fmt_name(is->audio_filter_src.fmt),
            is->audio_filter_src.channels,
            1, is->audio_filter_src.freq);
        if (is->audio_filter_src.channel_layout)
            snprintf(asrc_args + ret, sizeof(asrc_args) - ret,
                ":channel_layout=0x%"PRIx64, is->audio_filter_src.channel_layout);

        ret = avfilter_graph_create_filter(&filt_asrc,
            avfilter_get_by_name("abuffer"), "ffplay_abuffer",
            asrc_args, nullptr, is->agraph);
        if (ret < 0)
            goto end;


        ret = avfilter_graph_create_filter(&filt_asink,
            avfilter_get_by_name("abuffersink"), "ffplay_abuffersink",
            nullptr, nullptr, is->agraph);
        if (ret < 0)
            goto end;

        if ((ret = av_opt_set_int_list(filt_asink, "sample_fmts", sample_fmts, AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;

        if (force_output_format) {
            channel_layouts[0] = is->audio_tgt.channel_layout;
            channels[0] = is->audio_tgt.channels;
            sample_rates[0] = is->audio_tgt.freq;
            if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 0, AV_OPT_SEARCH_CHILDREN)) < 0)
                goto end;
            if ((ret = av_opt_set_int_list(filt_asink, "channel_layouts", channel_layouts, -1, AV_OPT_SEARCH_CHILDREN)) < 0)
                goto end;
            if ((ret = av_opt_set_int_list(filt_asink, "channel_counts", channels, -1, AV_OPT_SEARCH_CHILDREN)) < 0)
                goto end;
            if ((ret = av_opt_set_int_list(filt_asink, "sample_rates", sample_rates, -1, AV_OPT_SEARCH_CHILDREN)) < 0)
                goto end;
        }


        if ((ret = configure_filtergraph(is->agraph, afilters, filt_asrc, filt_asink)) < 0)
            goto end;

        is->in_audio_filter = filt_asrc;
        is->out_audio_filter = filt_asink;

    end:
        if (ret < 0)
            avfilter_graph_free(&is->agraph);
        return ret;
    }
#endif  /* CONFIG_AVFILTER */

    int audio_thread()
    {
        AVFrame* frame = av_frame_alloc();
        SFrame* af;
#if CONFIG_AVFILTER
        int last_serial = -1;
        int64_t dec_channel_layout;
        int reconfigure;
#endif
        int got_frame = 0;
        AVRational tb;
        int ret = 0;

        if (!frame)
            return AVERROR(ENOMEM);

        do {
            if ((got_frame = this->auddec.DecodeFrame(frame, nullptr)) < 0)
                goto the_end;

            if (got_frame) {
                tb = AVRational{ 1, frame->sample_rate };

#if CONFIG_AVFILTER
                dec_channel_layout = get_valid_channel_layout(frame->channel_layout, frame->channels);

                reconfigure =
                    cmp_audio_fmts(this->audio_filter_src.fmt, this->audio_filter_src.channels,
                        frame->format, frame->channels) ||
                    this->audio_filter_src.channel_layout != dec_channel_layout ||
                    this->audio_filter_src.freq != frame->sample_rate ||
                    this->auddec.pkt_serial != last_serial;

                if (reconfigure) {
                    char buf1[1024], buf2[1024];
                    av_get_channel_layout_string(buf1, sizeof(buf1), -1, this->audio_filter_src.channel_layout);
                    av_get_channel_layout_string(buf2, sizeof(buf2), -1, dec_channel_layout);
                    av_log(nullptr, AV_LOG_DEBUG,
                        "Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
                        this->audio_filter_src.freq, this->audio_filter_src.channels, av_get_sample_fmt_name(this->audio_filter_src.fmt), buf1, last_serial,
                        frame->sample_rate, frame->channels, av_get_sample_fmt_name(frame->format), buf2, this->auddec.pkt_serial);

                    this->audio_filter_src.fmt = frame->format;
                    this->audio_filter_src.channels = frame->channels;
                    this->audio_filter_src.channel_layout = dec_channel_layout;
                    this->audio_filter_src.freq = frame->sample_rate;
                    last_serial = this->auddec.pkt_serial;

                    if ((ret = configure_audio_filters(is, afilters, 1)) < 0)
                        goto the_end;
                }

                if ((ret = av_buffersrc_add_frame(this->in_audio_filter, frame)) < 0)
                    goto the_end;

                while ((ret = av_buffersink_get_frame_flags(this->out_audio_filter, frame, 0)) >= 0) {
                    tb = av_buffersink_get_time_base(this->out_audio_filter);
#endif
                    if (!(af = this->sampq.PeekWritable()))
                        goto the_end;

                    af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
                    af->pos = frame->pkt_pos;
                    af->serial = this->auddec.pkt_serial;
                    af->duration = av_q2d(AVRational{ frame->nb_samples, frame->sample_rate });

                    av_frame_move_ref(af->frame, frame);
                    this->sampq.Push();

#if CONFIG_AVFILTER
                    if (this->audioq.serial != this->auddec.pkt_serial)
                        break;
                }
                if (ret == AVERROR_EOF)
                    this->auddec.finished = this->auddec.pkt_serial;
#endif
            }
        } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
    the_end:
#if CONFIG_AVFILTER
        avfilter_graph_free(&this->agraph);
#endif
        av_frame_free(&frame);
        return ret;
    }

    int video_thread()
    {
        //FMediaPlayer* pPlayer = static_cast<FMediaPlayer*>(pUserData);
        AVFrame* frame = av_frame_alloc();
        double pts;
        double duration;
        int ret;
        AVRational tb = this->video_st->time_base;
        AVRational frame_rate = av_guess_frame_rate(this->ic, this->video_st, nullptr);

#if CONFIG_AVFILTER
        AVFilterGraph* graph = nullptr;
        AVFilterContext* filt_out = nullptr, * filt_in = nullptr;
        int last_w = 0;
        int last_h = 0;
        enum AVPixelFormat last_format = -2;
        int last_serial = -1;
        int last_vfilter_idx = 0;
#endif

        if (!frame)
            return AVERROR(ENOMEM);

        for (;;) {
            ret = this->get_video_frame(frame);
            if (ret < 0)
                goto the_end;
            if (!ret)
                continue;

#if CONFIG_AVFILTER
            if (last_w != frame->width
                || last_h != frame->height
                || last_format != frame->format
                || last_serial != this->viddec.pkt_serial
                || last_vfilter_idx != this->vfilter_idx) {
                av_log(nullptr, AV_LOG_DEBUG,
                    "Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s serial:%d\n",
                    last_w, last_h,
                    (const char*)av_x_if_null(av_get_pix_fmt_name(last_format), "none"), last_serial,
                    frame->width, frame->height,
                    (const char*)av_x_if_null(av_get_pix_fmt_name(frame->format), "none"), this->viddec.pkt_serial);
                avfilter_graph_free(&graph);
                graph = avfilter_graph_alloc();
                if (!graph) {
                    ret = AVERROR(ENOMEM);
                    goto the_end;
                }
                graph->nb_threads = filter_nbthreads;
                if ((ret = configure_video_filters(graph, is, vfilters_list ? vfilters_list[this->vfilter_idx] : nullptr, frame)) < 0) {
                    SDL_Event event;
                    event.type = FF_QUIT_EVENT;
                    event.user.data1 = is;
                    SDL_PushEvent(&event);
                    goto the_end;
                }
                filt_in = this->in_video_filter;
                filt_out = this->out_video_filter;
                last_w = frame->width;
                last_h = frame->height;
                last_format = frame->format;
                last_serial = this->viddec.pkt_serial;
                last_vfilter_idx = this->vfilter_idx;
                frame_rate = av_buffersink_get_frame_rate(filt_out);
            }

            ret = av_buffersrc_add_frame(filt_in, frame);
            if (ret < 0)
                goto the_end;

            while (ret >= 0) {
                this->frame_last_returned_time = av_gettime_relative() / 1000000.0;

                ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
                if (ret < 0) {
                    if (ret == AVERROR_EOF)
                        this->viddec.finished = this->viddec.pkt_serial;
                    ret = 0;
                    break;
                }

                this->frame_last_filter_delay = av_gettime_relative() / 1000000.0 - this->frame_last_returned_time;
                if (fabs(this->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0)
                    this->frame_last_filter_delay = 0;
                tb = av_buffersink_get_time_base(filt_out);
#endif
                duration = (frame_rate.num && frame_rate.den ? av_q2d(AVRational{ frame_rate.den, frame_rate.num }) : 0);
                pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
                ret = this->queue_picture(frame, pts, duration, frame->pkt_pos, this->viddec.pkt_serial);
                av_frame_unref(frame);
#if CONFIG_AVFILTER
                if (this->videoq.serial != this->viddec.pkt_serial)
                    break;
            }
#endif

            if (ret < 0)
                goto the_end;
        }
    the_end:
#if CONFIG_AVFILTER
        avfilter_graph_free(&graph);
#endif
        av_frame_free(&frame);
        return 0;
    }

    int subtitle_thread()
    {
        SFrame* sp = nullptr;
        int got_subtitle;
        double pts;

        for (;;) {
            if (!(sp = this->subpq.PeekWritable()))
                return 0;

            if ((got_subtitle = this->subdec.DecodeFrame(nullptr, &sp->sub)) < 0)
                break;

            pts = 0;

            if (got_subtitle && sp->sub.format == 0) {
                if (sp->sub.pts != AV_NOPTS_VALUE)
                    pts = sp->sub.pts / (double)AV_TIME_BASE;
                sp->pts = pts;
                sp->serial = this->subdec.pkt_serial;
                sp->width = this->subdec.avctx->width;
                sp->height = this->subdec.avctx->height;
                sp->uploaded = 0;

                /* now we can update the picture count */
                this->subpq.Push();
            }
            else if (got_subtitle) {
                avsubtitle_free(&sp->sub);
            }
        }
        return 0;
    }

    /* this thread gets the stream from the disk or the network */
    int read_thread()
    {
        AVFormatContext* ic = nullptr;
        int err, i, ret;
        int st_index[AVMEDIA_TYPE_NB];
        AVPacket pkt1, * pkt = &pkt1;
        int64_t stream_start_time;
        int pkt_in_play_range = 0;
        AVDictionaryEntry* t = nullptr;
        std::mutex wait_mutex;
        int scan_all_pmts_set = 0;
        int64_t pkt_ts;

        memset(st_index, -1, sizeof(st_index));
        this->last_video_stream = this->video_stream = -1;
        this->last_audio_stream = this->audio_stream = -1;
        this->last_subtitle_stream = this->subtitle_stream = -1;
        this->eof = 0;

        ic = avformat_alloc_context();
        if (!ic) {
            av_log(nullptr, AV_LOG_FATAL, "Could not allocate context.\n");
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        ic->interrupt_callback.callback = decode_interrupt_cb;
        ic->interrupt_callback.opaque = this;
        if (!av_dict_get(format_opts, "scan_all_pmts", nullptr, AV_DICT_MATCH_CASE)) {
            av_dict_set(&format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
            scan_all_pmts_set = 1;
        }
        err = avformat_open_input(&ic, this->filename, this->iformat, &format_opts);
        if (err < 0) {
            print_error(this->filename, err);
            ret = -1;
            goto fail;
        }
        if (scan_all_pmts_set)
            av_dict_set(&format_opts, "scan_all_pmts", nullptr, AV_DICT_MATCH_CASE);

        if ((t = av_dict_get(format_opts, "", nullptr, AV_DICT_IGNORE_SUFFIX))) {
            av_log(nullptr, AV_LOG_ERROR, "Option %s not found.\n", t->key);
            ret = AVERROR_OPTION_NOT_FOUND;
            goto fail;
        }
        this->ic = ic;

        /*if (genpts)
            ic->flags |= AVFMT_FLAG_GENPTS;*/

        av_format_inject_global_side_data(ic);

        if (find_stream_info) {
            AVDictionary** opts = setup_find_stream_info_opts(ic, codec_opts);
            int orig_nb_streams = ic->nb_streams;

            err = avformat_find_stream_info(ic, opts);

            for (i = 0; i < orig_nb_streams; i++)
                av_dict_free(&opts[i]);
            av_freep(&opts);

            if (err < 0) {
                av_log(nullptr, AV_LOG_WARNING, "%s: could not find codec parameters\n", this->filename);
                ret = -1;
                goto fail;
            }
        }

        if (ic->pb)
            ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end

        if (this->seek_by_bytes < 0)
            this->seek_by_bytes = !!(ic->iformat->flags & AVFMT_TS_DISCONT) && strcmp("ogg", ic->iformat->name);

        this->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

        /* if seeking requested, we execute it */
        if (this->start_time != AV_NOPTS_VALUE) {
            int64_t timestamp;

            timestamp = this->start_time;
            /* add the stream start time */
            if (ic->start_time != AV_NOPTS_VALUE)
                timestamp += ic->start_time;
            ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
            if (ret < 0) {
                av_log(nullptr, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
                    this->filename, (double)timestamp / AV_TIME_BASE);
            }
        }

        this->realtime = this->is_realtime();

        if (show_status)
            av_dump_format(ic, 0, this->filename, 0);

        for (i = 0; i < ic->nb_streams; i++) {
            AVStream* st = ic->streams[i];
            enum AVMediaType type = st->codecpar->codec_type;
            st->discard = AVDISCARD_ALL;
            if (type >= 0 && wanted_stream_spec[type] && st_index[type] == -1)
                if (avformat_match_stream_specifier(ic, st, wanted_stream_spec[type]) > 0)
                    st_index[type] = i;
        }
        for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
            if (wanted_stream_spec[i] && st_index[i] == -1) {
                av_log(nullptr, AV_LOG_ERROR, "Stream specifier %s does not match any %s stream\n", wanted_stream_spec[i], av_get_media_type_string(static_cast<AVMediaType>(i)));
                st_index[i] = INT_MAX;
            }
        }

        st_index[AVMEDIA_TYPE_VIDEO] = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,st_index[AVMEDIA_TYPE_VIDEO], -1, nullptr, 0);
        st_index[AVMEDIA_TYPE_AUDIO] = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, st_index[AVMEDIA_TYPE_AUDIO], st_index[AVMEDIA_TYPE_VIDEO],nullptr, 0);
        st_index[AVMEDIA_TYPE_SUBTITLE] = av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE, st_index[AVMEDIA_TYPE_SUBTITLE],
                (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ? st_index[AVMEDIA_TYPE_AUDIO] : st_index[AVMEDIA_TYPE_VIDEO]),
                nullptr, 0);

        if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
            AVStream* st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
            AVCodecParameters* codecpar = st->codecpar;
            AVRational sar = av_guess_sample_aspect_ratio(ic, st, nullptr);
            if (codecpar->width)
                this->set_default_window_size(codecpar->width, codecpar->height, sar);
        }

        /* open the streams */
        if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
            this->stream_component_open(st_index[AVMEDIA_TYPE_AUDIO]);
        }

        ret = -1;
        if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
            ret = this->stream_component_open(st_index[AVMEDIA_TYPE_VIDEO]);
        }
        if (this->eShow_mode == FMediaPlayer::EShowMode::SHOW_MODE_NONE)
            this->eShow_mode = ret >= 0 ? FMediaPlayer::EShowMode::SHOW_MODE_VIDEO : FMediaPlayer::EShowMode::SHOW_MODE_RDFT;

        if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
            this->stream_component_open(st_index[AVMEDIA_TYPE_SUBTITLE]);
        }

        if (this->video_stream < 0 && this->audio_stream < 0) {
            av_log(nullptr, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n",
                this->filename);
            ret = -1;
            goto fail;
        }

        if (this->infinite_buffer < 0 && this->realtime)
            this->infinite_buffer = 1;

        for (;;) {
            if (this->abort_request)
                break;
            if (this->paused != this->last_paused) {
                this->last_paused = this->paused;
                if (this->paused)
                    this->read_pause_return = av_read_pause(ic);
                else
                    av_read_play(ic);
            }
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
            if (this->paused &&
                (!strcmp(ic->iformat->name, "rtsp") ||
                (ic->pb && !strncmp(this->filename, "mmsh:", 5)))) {
                /* wait 10 ms to avoid trying to get another packet */
                /* XXX: horrible */
                SDL_Delay(10);
                continue;
            }
#endif
            if (this->seek_req) {
                int64_t seek_target = this->seek_pos;
                int64_t seek_min = this->seek_rel > 0 ? seek_target - this->seek_rel + 2 : INT64_MIN;
                int64_t seek_max = this->seek_rel < 0 ? seek_target - this->seek_rel - 2 : INT64_MAX;
                // FIXME the +-2 pPlayer due to rounding being not done in the correct direction in generation
                //      of the seek_pos/seek_rel variables

                ret = avformat_seek_file(this->ic, -1, seek_min, seek_target, seek_max, this->seek_flags);
                if (ret < 0) {
                    av_log(nullptr, AV_LOG_ERROR,
                        "%s: error while seeking\n", this->ic->url);
                }
                else {
                    if (this->audio_stream >= 0) {
                        this->audioq.Flush();
                        this->audioq.Put(&/*this->*/flush_pkt);
                    }
                    if (this->subtitle_stream >= 0) {
                        this->subtitleq.Flush();
                        this->subtitleq.Put(&/*this->*/flush_pkt);
                    }
                    if (this->video_stream >= 0) {
                        this->videoq.Flush();
                        this->videoq.Put(&/*this->*/flush_pkt);
                    }
                    if (this->seek_flags & AVSEEK_FLAG_BYTE) {
                        this->extclk.Set(NAN, 0);
                    }
                    else {
                        this->extclk.Set(seek_target / (double)AV_TIME_BASE, 0);
                    }
                }
                this->seek_req = 0;
                this->queue_attachments_req = 1;
                this->eof = 0;
                if (this->paused)
                    this->step_to_next_frame();
            }
            if (this->queue_attachments_req) {
                if (this->video_st && this->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                    AVPacket copy = { 0 };
                    if ((ret = av_packet_ref(&copy, &this->video_st->attached_pic)) < 0)
                        goto fail;
                    this->videoq.Put(&copy);
                    this->videoq.PutNullPacket(this->video_stream);
                }
                this->queue_attachments_req = 0;
            }

            /* if the queue are full, no need to read more */
            if (this->infinite_buffer < 1 &&
                (this->audioq.size + this->videoq.size + this->subtitleq.size > MAX_QUEUE_SIZE
                    || (this->stream_has_enough_packets(this->audio_st, this->audio_stream, &this->audioq) &&
                        this->stream_has_enough_packets(this->video_st, this->video_stream, &this->videoq) &&
                        this->stream_has_enough_packets(this->subtitle_st, this->subtitle_stream, &this->subtitleq)))) {
                /* wait 10 ms */
                std::unique_lock<std::mutex> lock(wait_mutex);
                this->continue_read_thread->wait_for(lock, std::chrono::milliseconds(10));
                continue;
            }
            if (!this->paused &&
                (!this->audio_st || (this->auddec.finished == this->audioq.serial && this->sampq.NbRemaining() == 0)) &&
                (!this->video_st || (this->viddec.finished == this->videoq.serial && this->pictq.NbRemaining() == 0))) {
                if (this->loop != 1 && (!this->loop || --this->loop))
                    this->stream_seek(this->start_time != AV_NOPTS_VALUE ? this->start_time : 0, 0, 0);
                
                else if (autoexit) {
                    ret = AVERROR_EOF;
                    goto fail;
                }
            }
            ret = av_read_frame(ic, pkt);
            if (ret < 0) {
                if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !this->eof) {
                    if (this->video_stream >= 0)
                        this->videoq.PutNullPacket(this->video_stream);
                    if (this->audio_stream >= 0)
                        this->audioq.PutNullPacket(this->audio_stream);
                    if (this->subtitle_stream >= 0)
                        this->subtitleq.PutNullPacket(this->subtitle_stream);
                    this->eof = 1;
                }
                if (ic->pb && ic->pb->error)
                    break;

                std::unique_lock<std::mutex> lock(wait_mutex);
                this->continue_read_thread->wait_for(lock, std::chrono::milliseconds(10));
                continue;
            }
            else {
                this->eof = 0;
            }
            /* check if packet pPlayer in play range specified by user, then queue, otherwise discard */
            stream_start_time = ic->streams[pkt->stream_index]->start_time;
            pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
            pkt_in_play_range = this->duration == AV_NOPTS_VALUE ||
                (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                av_q2d(ic->streams[pkt->stream_index]->time_base) -
                (double)(this->start_time != AV_NOPTS_VALUE ? this->start_time : 0) / 1000000
                <= ((double)this->duration / 1000000);
            if (pkt->stream_index == this->audio_stream && pkt_in_play_range) {
                this->audioq.Put(pkt);
            }
            else if (pkt->stream_index == this->video_stream && pkt_in_play_range
                && !(this->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
                this->videoq.Put(pkt);
            }
            else if (pkt->stream_index == this->subtitle_stream && pkt_in_play_range) {
                this->subtitleq.Put(pkt);
            }
            else {
                av_packet_unref(pkt);
            }
        }

        ret = 0;
    fail:
        if (ic && !this->ic)
            avformat_close_input(&ic);

        if (ret != 0) {
            SDL_Event event;

            event.type = FF_QUIT_EVENT;
            event.user.data1 = this;//pPlayer;
            SDL_PushEvent(&event);
        }
        return 0;
    }

    int audio_open(int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams* audio_hw_params)
    {
        SDL_AudioSpec wanted_spec, spec;
        const char* env;
        static const int next_nb_channels[] = { 0, 0, 1, 6, 2, 6, 4, 6 };
        static const int next_sample_rates[] = { 0, 44100, 48000, 96000, 192000 };
        int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;

        env = SDL_getenv("SDL_AUDIO_CHANNELS");
        if (env) {
            wanted_nb_channels = atoi(env);
            wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
        }
        if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
            wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
            wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
        }
        wanted_nb_channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
        wanted_spec.channels = wanted_nb_channels;
        wanted_spec.freq = wanted_sample_rate;
        if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
            av_log(nullptr, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
            return -1;
        }
        while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
            next_sample_rate_idx--;
        wanted_spec.format = AUDIO_S16SYS;
        wanted_spec.silence = 0;
        wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
        wanted_spec.callback = sdl_audio_callback;
        wanted_spec.userdata = this;
        while (!(audio_dev = SDL_OpenAudioDevice(nullptr, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
            av_log(nullptr, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
                wanted_spec.channels, wanted_spec.freq, SDL_GetError());
            wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
            if (!wanted_spec.channels) {
                wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
                wanted_spec.channels = wanted_nb_channels;
                if (!wanted_spec.freq) {
                    av_log(nullptr, AV_LOG_ERROR,
                        "No more combinations to try, audio open failed\n");
                    return -1;
                }
            }
            wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
        }
        if (spec.format != AUDIO_S16SYS) {
            av_log(nullptr, AV_LOG_ERROR,
                "SDL advised audio format %d is not supported!\n", spec.format);
            return -1;
        }
        if (spec.channels != wanted_spec.channels) {
            wanted_channel_layout = av_get_default_channel_layout(spec.channels);
            if (!wanted_channel_layout) {
                av_log(nullptr, AV_LOG_ERROR,
                    "SDL advised channel count %d is not supported!\n", spec.channels);
                return -1;
            }
        }

        audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
        audio_hw_params->freq = spec.freq;
        audio_hw_params->channel_layout = wanted_channel_layout;
        audio_hw_params->channels = spec.channels;
        audio_hw_params->frame_size = av_samples_get_buffer_size(nullptr, audio_hw_params->channels, 1, audio_hw_params->fmt, 1);
        audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(nullptr, audio_hw_params->channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
        if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
            av_log(nullptr, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
            return -1;
        }
        return spec.size;
    }

    /* prepare a new audio buffer */
    static void sdl_audio_callback(void* pUserData, Uint8* stream, int len)
    {
        FMediaPlayer* pPlayer = static_cast<FMediaPlayer*>(pUserData);
        int audio_size, len1;

        pPlayer->audio_callback_time = av_gettime_relative();

        while (len > 0) {
            if (pPlayer->audio_buf_index >= pPlayer->audio_buf_size) {
                audio_size = pPlayer->audio_decode_frame();
                if (audio_size < 0) {
                    /* if error, just output silence */
                    pPlayer->audio_buf = nullptr;
                    pPlayer->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / pPlayer->audio_tgt.frame_size * pPlayer->audio_tgt.frame_size;
                }
                else {
                    if (pPlayer->eShow_mode != FMediaPlayer::EShowMode::SHOW_MODE_VIDEO)
                        pPlayer->update_sample_display((int16_t*)pPlayer->audio_buf, audio_size);
                    pPlayer->audio_buf_size = audio_size;
                }
                pPlayer->audio_buf_index = 0;
            }
            len1 = pPlayer->audio_buf_size - pPlayer->audio_buf_index;
            if (len1 > len)
                len1 = len;
            if (!pPlayer->muted && pPlayer->audio_buf && pPlayer->audio_volume == SDL_MIX_MAXVOLUME)
                memcpy(stream, (uint8_t*)pPlayer->audio_buf + pPlayer->audio_buf_index, len1);
            else {
                memset(stream, 0, len1);
                if (!pPlayer->muted && pPlayer->audio_buf)
                    SDL_MixAudioFormat(stream, (uint8_t*)pPlayer->audio_buf + pPlayer->audio_buf_index, AUDIO_S16SYS, len1, pPlayer->audio_volume);
            }
            len -= len1;
            stream += len1;
            pPlayer->audio_buf_index += len1;
        }
        pPlayer->audio_write_buf_size = pPlayer->audio_buf_size - pPlayer->audio_buf_index;
        /* Let's assume the audio driver that is used by SDL has two periods. */
        if (!isnan(pPlayer->audio_clock)) {
            pPlayer->audclk.SetAt(pPlayer->audio_clock - (double)(2 * pPlayer->audio_hw_buf_size + pPlayer->audio_write_buf_size) / pPlayer->audio_tgt.bytes_per_sec, pPlayer->audio_clock_serial, pPlayer->audio_callback_time / 1000000.0);
            sync_clock_to_slave(&pPlayer->extclk, &pPlayer->audclk);
        }
    }

    static int decode_interrupt_cb(void* pUserData)
    {
        FMediaPlayer* pPlayer = static_cast<FMediaPlayer*>(pUserData);
        return pPlayer->abort_request;
    }

    /* copy samples for viewing in editor window */
    void update_sample_display(short* samples, int samples_size)
    {
        int size, len;

        size = samples_size / sizeof(short);
        while (size > 0) {
            len = SAMPLE_ARRAY_SIZE - this->sample_array_index;
            if (len > size)
                len = size;
            memcpy(this->sample_array + this->sample_array_index, samples, len * sizeof(short));
            samples += len;
            this->sample_array_index += len;
            if (this->sample_array_index >= SAMPLE_ARRAY_SIZE)
                this->sample_array_index = 0;
            size -= len;
        }
    }

    /* return the wanted number of samples to get better sync if sync_type is video
     * or external master clock */
    int synchronize_audio(int nb_samples)
    {
        int wanted_nb_samples = nb_samples;

        /* if not master, then we try to remove or add samples to correct the clock */
        if (get_master_sync_type() != ESyncType::AV_SYNC_AUDIO_MASTER) {
            double diff, avg_diff;
            int min_nb_samples, max_nb_samples;

            diff = this->audclk.Get() - get_master_clock();

            if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
                this->audio_diff_cum = diff + this->audio_diff_avg_coef * this->audio_diff_cum;
                if (this->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                    /* not enough measures to have a correct estimate */
                    this->audio_diff_avg_count++;
                }
                else {
                    /* estimate the A-V difference */
                    avg_diff = this->audio_diff_cum * (1.0 - this->audio_diff_avg_coef);

                    if (fabs(avg_diff) >= this->audio_diff_threshold) {
                        wanted_nb_samples = nb_samples + (int)(diff * this->audio_src.freq);
                        min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                        max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                        wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
                    }
                    av_log(nullptr, AV_LOG_TRACE, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
                        diff, avg_diff, wanted_nb_samples - nb_samples,
                        this->audio_clock, this->audio_diff_threshold);
                }
            }
            else {
                /* too big difference : may be initial PTS errors, so
                   reset A-V filter */
                this->audio_diff_avg_count = 0;
                this->audio_diff_cum = 0;
            }
        }

        return wanted_nb_samples;
    }

    /**
     * Decode one audio frame and return its uncompressed size.
     *
     * The processed audio frame is decoded, converted if required, and
     * stored in is->audio_buf, with size in bytes given by the return
     * value.
     */
    int audio_decode_frame()
    {
        int data_size, resampled_data_size;
        int64_t dec_channel_layout;
        av_unused double audio_clock0;
        int wanted_nb_samples;
        SFrame* af;

        if (this->paused)
            return -1;

        do {
#if defined(_WIN32)
            while (this->sampq.NbRemaining() == 0) {
                if ((av_gettime_relative() - audio_callback_time) > 1000000LL * this->audio_hw_buf_size / this->audio_tgt.bytes_per_sec / 2)
                    return -1;
                av_usleep(1000);
            }
#endif
            if (!(af = this->sampq.PeekReadable()))
                return -1;
            this->sampq.Next();
        } while (af->serial != this->audioq.serial);

        data_size = av_samples_get_buffer_size(nullptr, af->frame->channels, af->frame->nb_samples, static_cast<AVSampleFormat>(af->frame->format), 1);

        dec_channel_layout =
            (af->frame->channel_layout && af->frame->channels == av_get_channel_layout_nb_channels(af->frame->channel_layout)) ?
            af->frame->channel_layout : av_get_default_channel_layout(af->frame->channels);
        wanted_nb_samples = synchronize_audio(af->frame->nb_samples);

        if (af->frame->format != this->audio_src.fmt ||
            dec_channel_layout != this->audio_src.channel_layout ||
            af->frame->sample_rate != this->audio_src.freq ||
            (wanted_nb_samples != af->frame->nb_samples && !this->swr_ctx)) {
            swr_free(&this->swr_ctx);
            this->swr_ctx = swr_alloc_set_opts(nullptr,
                this->audio_tgt.channel_layout, this->audio_tgt.fmt, this->audio_tgt.freq,
                dec_channel_layout, static_cast<AVSampleFormat>(af->frame->format), af->frame->sample_rate,
                0, nullptr);
            if (!this->swr_ctx || swr_init(this->swr_ctx) < 0) {
                av_log(nullptr, AV_LOG_ERROR,
                    "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                    af->frame->sample_rate, av_get_sample_fmt_name(static_cast<AVSampleFormat>(af->frame->format)), af->frame->channels,
                    this->audio_tgt.freq, av_get_sample_fmt_name(this->audio_tgt.fmt), this->audio_tgt.channels);
                swr_free(&this->swr_ctx);
                return -1;
            }
            this->audio_src.channel_layout = dec_channel_layout;
            this->audio_src.channels = af->frame->channels;
            this->audio_src.freq = af->frame->sample_rate;
            this->audio_src.fmt = static_cast<AVSampleFormat>(af->frame->format);
        }

        if (this->swr_ctx) {
            const uint8_t** in = (const uint8_t**)af->frame->extended_data;
            uint8_t** out = &this->audio_buf1;
            int out_count = (int64_t)wanted_nb_samples * this->audio_tgt.freq / af->frame->sample_rate + 256;
            int out_size = av_samples_get_buffer_size(nullptr, this->audio_tgt.channels, out_count, this->audio_tgt.fmt, 0);
            int len2;
            if (out_size < 0) {
                av_log(nullptr, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
                return -1;
            }
            if (wanted_nb_samples != af->frame->nb_samples) {
                if (swr_set_compensation(this->swr_ctx, (wanted_nb_samples - af->frame->nb_samples) * this->audio_tgt.freq / af->frame->sample_rate,
                    wanted_nb_samples * this->audio_tgt.freq / af->frame->sample_rate) < 0) {
                    av_log(nullptr, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                    return -1;
                }
            }
            av_fast_malloc(&this->audio_buf1, &this->audio_buf1_size, out_size);
            if (!this->audio_buf1)
                return AVERROR(ENOMEM);
            len2 = swr_convert(this->swr_ctx, out, out_count, in, af->frame->nb_samples);
            if (len2 < 0) {
                av_log(nullptr, AV_LOG_ERROR, "swr_convert() failed\n");
                return -1;
            }
            if (len2 == out_count) {
                av_log(nullptr, AV_LOG_WARNING, "audio buffer is probably too small\n");
                if (swr_init(this->swr_ctx) < 0)
                    swr_free(&this->swr_ctx);
            }
            this->audio_buf = this->audio_buf1;
            resampled_data_size = len2 * this->audio_tgt.channels * av_get_bytes_per_sample(this->audio_tgt.fmt);
        }
        else {
            this->audio_buf = af->frame->data[0];
            resampled_data_size = data_size;
        }

        audio_clock0 = this->audio_clock;
        /* update the audio clock with the pts */
        if (!isnan(af->pts))
            this->audio_clock = af->pts + (double)af->frame->nb_samples / af->frame->sample_rate;
        else
            this->audio_clock = NAN;
        this->audio_clock_serial = af->serial;
#ifdef DEBUG
        {
            static double last_clock;
            printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
                this->audio_clock - last_clock,
                this->audio_clock, audio_clock0);
            last_clock = this->audio_clock;
        }
#endif
        return resampled_data_size;
    }

    /* open a given stream. Return 0 if OK */
    int stream_component_open(int stream_index)
    {
        AVCodecContext* avctx;
        AVCodec* codec;
        const char* forced_codec_name = nullptr;
        AVDictionary* opts = nullptr;
        AVDictionaryEntry* t = nullptr;
        int sample_rate, nb_channels;
        int64_t channel_layout;
        int ret = 0;
        int stream_lowres = lowres;

        if (stream_index < 0 || stream_index >= ic->nb_streams)
            return -1;

        avctx = avcodec_alloc_context3(nullptr);
        if (!avctx)
            return AVERROR(ENOMEM);

        ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
        if (ret < 0)
            goto fail;
        avctx->pkt_timebase = ic->streams[stream_index]->time_base;

        codec = avcodec_find_decoder(avctx->codec_id);

        switch (avctx->codec_type) {
        case AVMEDIA_TYPE_AUDIO: this->last_audio_stream = stream_index; break;
        case AVMEDIA_TYPE_SUBTITLE: this->last_subtitle_stream = stream_index; break;
        case AVMEDIA_TYPE_VIDEO: this->last_video_stream = stream_index; break;
        }

        avctx->codec_id = codec->id;
        if (stream_lowres > codec->max_lowres) {
            av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n", codec->max_lowres);
            stream_lowres = codec->max_lowres;
        }
        avctx->lowres = stream_lowres;

        avctx->flags2 |= AV_CODEC_FLAG2_FAST;

        opts = filter_codec_opts(codec_opts, avctx->codec_id, ic, ic->streams[stream_index], codec);
        if (!av_dict_get(opts, "threads", nullptr, 0))
            av_dict_set(&opts, "threads", "auto", 0);
        if (stream_lowres)
            av_dict_set_int(&opts, "lowres", stream_lowres, 0);
        if (avctx->codec_type == AVMEDIA_TYPE_VIDEO || avctx->codec_type == AVMEDIA_TYPE_AUDIO)
            av_dict_set(&opts, "refcounted_frames", "1", 0);
        if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
            goto fail;
        }
        if ((t = av_dict_get(opts, "", nullptr, AV_DICT_IGNORE_SUFFIX))) {
            av_log(nullptr, AV_LOG_ERROR, "Option %s not found.\n", t->key);
            ret = AVERROR_OPTION_NOT_FOUND;
            goto fail;
        }

        this->eof = 0;
        ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
        switch (avctx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
#if CONFIG_AVFILTER
        {
            AVFilterContext* sink;

            this->audio_filter_src.freq = avctx->sample_rate;
            this->audio_filter_src.channels = avctx->channels;
            this->audio_filter_src.channel_layout = get_valid_channel_layout(avctx->channel_layout, avctx->channels);
            this->audio_filter_src.fmt = avctx->sample_fmt;
            if ((ret = configure_audio_filters(is, afilters, 0)) < 0)
                goto fail;
            sink = this->out_audio_filter;
            sample_rate = av_buffersink_get_sample_rate(sink);
            nb_channels = av_buffersink_get_channels(sink);
            channel_layout = av_buffersink_get_channel_layout(sink);
        }
#else
        {
            sample_rate = avctx->sample_rate;
            nb_channels = avctx->channels;
            channel_layout = avctx->channel_layout;
#endif

            /* prepare audio output */
            if ((ret = audio_open(channel_layout, nb_channels, sample_rate, &this->audio_tgt)) < 0)
                goto fail;
            this->audio_hw_buf_size = ret;
            this->audio_src = this->audio_tgt;
            this->audio_buf_size = 0;
            this->audio_buf_index = 0;

            /* init averaging filter */
            this->audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
            this->audio_diff_avg_count = 0;
            /* since we do not have a precise anough audio FIFO fullness,
               we correct audio sync only if larger than this threshold */
            this->audio_diff_threshold = (double)(this->audio_hw_buf_size) / this->audio_tgt.bytes_per_sec;

            this->audio_stream = stream_index;
            this->audio_st = ic->streams[stream_index];

            this->auddec.Init(avctx, &this->audioq, this->continue_read_thread);
            if ((this->ic->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)) && !this->ic->iformat->read_seek) {
                this->auddec.start_pts = this->audio_st->start_time;
                this->auddec.start_pts_tb = this->audio_st->time_base;
            }
            std::function<int(void)> audioFunc = std::bind(&FMediaPlayer::audio_thread, this);
            if ((ret = this->auddec.Start(audioFunc)) < 0)
                goto out;
            SDL_PauseAudioDevice(audio_dev, 0);
        }
            break;
        case AVMEDIA_TYPE_VIDEO:
            {
                this->video_stream = stream_index;
                this->video_st = ic->streams[stream_index];

                this->viddec.Init(avctx, &this->videoq, this->continue_read_thread);
                std::function<int(void)> videoFunc = std::bind(&FMediaPlayer::video_thread, this);
                if ((ret = this->viddec.Start(videoFunc)) < 0)
                    goto out;
                this->queue_attachments_req = 1;
            }
            break;
        case AVMEDIA_TYPE_SUBTITLE:
        {
            this->subtitle_stream = stream_index;
            this->subtitle_st = ic->streams[stream_index];

            this->subdec.Init(avctx, &this->subtitleq, this->continue_read_thread);
            std::function<int(void)> subtitleoFunc = std::bind(&FMediaPlayer::subtitle_thread, this);
            if ((ret = this->subdec.Start(subtitleoFunc)) < 0)
                goto out;
        }
            break;
        default:
            break;
        }
        goto out;

    fail:
        avcodec_free_context(&avctx);
    out:
        av_dict_free(&opts);

        return ret;
    }

    int stream_has_enough_packets(AVStream* st, int stream_id, PacketQueue* queue) 
    {
        return stream_id < 0 ||
            queue->abort_request ||
            (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
            queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0);
    }

    int is_realtime()
    {
        if (!strcmp(ic->iformat->name, "rtp") || !strcmp(ic->iformat->name, "rtsp") || !strcmp(ic->iformat->name, "sdp"))
            return 1;

        if (ic->pb && (!strncmp(ic->url, "rtp:", 4) || !strncmp(ic->url, "udp:", 4)))
            return 1;

        return 0;
    }

    bool stream_open(const char* filename, AVInputFormat* iformat)
    {
        av_init_packet(&flush_pkt);
        flush_pkt.data = (uint8_t*)&flush_pkt;

        std::function<int(void)> readFunc = std::bind(&FMediaPlayer::read_thread, this);

        this->filename = av_strdup(filename);
        if (!this->filename)
            goto fail;
        this->iformat = iformat;
        this->ytop = 0;
        this->xleft = 0;

        /* start video display */
        if (this->pictq.Init(&this->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
            goto fail;
        if (this->subpq.Init(&this->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
            goto fail;
        if (this->sampq.Init(&this->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
            goto fail;

        if (this->videoq.Init() < 0 ||
            this->audioq.Init() < 0 ||
            this->subtitleq.Init() < 0)
            goto fail;

        this->continue_read_thread = std::make_shared<std::condition_variable>();
        if (this->continue_read_thread == nullptr)
        {
            av_log(nullptr, AV_LOG_ERROR, "Make a condition varibale failed\n");
            return false;
        }

        this->vidclk.Init(&this->videoq.serial);
        this->audclk.Init(&this->audioq.serial);
        this->extclk.Init(&this->extclk.serial);
        this->audio_clock_serial = -1;
        if (startup_volume < 0)
            av_log(nullptr, AV_LOG_WARNING, "-volume=%d < 0, setting to 0\n", startup_volume);
        if (startup_volume > 100)
            av_log(nullptr, AV_LOG_WARNING, "-volume=%d > 100, setting to 100\n", startup_volume);
        startup_volume = av_clip(startup_volume, 0, 100);
        startup_volume = av_clip(SDL_MIX_MAXVOLUME * startup_volume / 100, 0, SDL_MIX_MAXVOLUME);
        this->audio_volume = startup_volume;
        this->muted = 0;
        this->av_sync_type = av_sync_type;
        
        this->future = std::async(std::launch::async, std::move(readFunc));
        if (!this->future.valid()) {
            av_log(nullptr, AV_LOG_FATAL, "CreateThread Failured\n");
        fail:
            stream_close();
            return false;
        }
        return true;
    }

    void stream_cycle_channel(int codec_type)
    {
        int start_index, stream_index;
        int old_index;
        AVStream* st;
        AVProgram* p = nullptr;
        int nb_streams = ic->nb_streams;

        if (codec_type == AVMEDIA_TYPE_VIDEO) {
            start_index = this->last_video_stream;
            old_index = this->video_stream;
        }
        else if (codec_type == AVMEDIA_TYPE_AUDIO) {
            start_index = this->last_audio_stream;
            old_index = this->audio_stream;
        }
        else {
            start_index = this->last_subtitle_stream;
            old_index = this->subtitle_stream;
        }
        stream_index = start_index;

        if (codec_type != AVMEDIA_TYPE_VIDEO && this->video_stream != -1) {
            p = av_find_program_from_stream(ic, nullptr, this->video_stream);
            if (p) {
                nb_streams = p->nb_stream_indexes;
                for (start_index = 0; start_index < nb_streams; start_index++)
                    if (p->stream_index[start_index] == stream_index)
                        break;
                if (start_index == nb_streams)
                    start_index = -1;
                stream_index = start_index;
            }
        }

        for (;;) {
            if (++stream_index >= nb_streams)
            {
                if (codec_type == AVMEDIA_TYPE_SUBTITLE)
                {
                    stream_index = -1;
                    this->last_subtitle_stream = -1;
                    goto the_end;
                }
                if (start_index == -1)
                    return;
                stream_index = 0;
            }
            if (stream_index == start_index)
                return;
            st = this->ic->streams[p ? p->stream_index[stream_index] : stream_index];
            if (st->codecpar->codec_type == codec_type) {
                /* check that parameters are OK */
                switch (codec_type) {
                case AVMEDIA_TYPE_AUDIO:
                    if (st->codecpar->sample_rate != 0 &&
                        st->codecpar->channels != 0)
                        goto the_end;
                    break;
                case AVMEDIA_TYPE_VIDEO:
                case AVMEDIA_TYPE_SUBTITLE:
                    goto the_end;
                default:
                    break;
                }
            }
        }
    the_end:
        if (p && stream_index != -1)
            stream_index = p->stream_index[stream_index];
        av_log(nullptr, AV_LOG_INFO, "Switch %s stream from #%d to #%d\n", av_get_media_type_string(static_cast<AVMediaType>(codec_type)), old_index, stream_index);

        stream_component_close(old_index);
        stream_component_open(stream_index);
    }

    void toggle_full_screen()
    {
        is_full_screen = !is_full_screen;
        SDL_SetWindowFullscreen(window, is_full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    }

    void toggle_audio_display()
    {
        int next = static_cast<int>(this->eShow_mode);
        do {
            next = (next + 1) % int(FMediaPlayer::EShowMode::SHOW_MODE_NB);
        } while (next != int(this->eShow_mode) && (next == int(FMediaPlayer::EShowMode::SHOW_MODE_VIDEO) && !this->video_st || next != int(FMediaPlayer::EShowMode::SHOW_MODE_VIDEO) && !this->audio_st));
        if (int(this->eShow_mode) != next) {
            this->force_refresh = 1;
            this->eShow_mode = FMediaPlayer::EShowMode(next);
        }
    }

    void refresh_loop_wait_event(SDL_Event* event) 
    {
        double remaining_time = 0.0;
        SDL_PumpEvents();
        while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
            if (!cursor_hidden && av_gettime_relative() - cursor_last_shown > CURSOR_HIDE_DELAY) {
                SDL_ShowCursor(0);
                cursor_hidden = 1;
            }
            if (remaining_time > 0.0)
                av_usleep((int64_t)(remaining_time * 1000000.0));
            remaining_time = REFRESH_RATE;
            if (this->eShow_mode != FMediaPlayer::EShowMode::SHOW_MODE_NONE && (!this->paused || this->force_refresh))
                video_refresh(&remaining_time);
            SDL_PumpEvents();
        }
    }

    void seek_chapter(int incr)
    {
        int64_t pos = get_master_clock() * AV_TIME_BASE;
        int i;

        if (!ic->nb_chapters)
            return;

        /* find the current chapter */
        for (i = 0; i < this->ic->nb_chapters; i++) {
            AVChapter* ch = this->ic->chapters[i];
            if (av_compare_ts(pos, AVRational{ 1, AV_TIME_BASE }, ch->start, ch->time_base) < 0) {
                i--;
                break;
            }
        }

        i += incr;
        i = FFMAX(i, 0);
        if (i >= this->ic->nb_chapters)
            return;

        av_log(nullptr, AV_LOG_VERBOSE, "Seeking to chapter %d.\n", i);
        stream_seek(av_rescale_q(this->ic->chapters[i]->start, this->ic->chapters[i]->time_base, AVRational{ 1, AV_TIME_BASE }), 0, 0);
    }

    /* handle an event sent by the GUI */
    void event_loop()
    {
        SDL_Event event;
        double incr, pos, frac;

        for (;;) {
            double x;
            refresh_loop_wait_event(&event);
            switch (event.type) {
            case SDL_KEYDOWN:
                if (exit_on_keydown || event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) {
                    do_exit();
                    break;
                }
                // If we don't yet have a window, skip all key events, because read_thread might still be initializing...
                if (!this->width)
                    continue;
                switch (event.key.keysym.sym) {
                case SDLK_f:
                    toggle_full_screen();
                    this->force_refresh = 1;
                    break;
                case SDLK_p:
                case SDLK_SPACE:
                    toggle_pause();
                    break;
                case SDLK_m:
                    toggle_mute();
                    break;
                case SDLK_KP_MULTIPLY:
                case SDLK_0:
                    update_volume(1, SDL_VOLUME_STEP);
                    break;
                case SDLK_KP_DIVIDE:
                case SDLK_9:
                    update_volume(-1, SDL_VOLUME_STEP);
                    break;
                case SDLK_s: // S: Step to next frame
                    step_to_next_frame();
                    break;
                case SDLK_a:
                    stream_cycle_channel(AVMEDIA_TYPE_AUDIO);
                    break;
                case SDLK_v:
                    stream_cycle_channel(AVMEDIA_TYPE_VIDEO);
                    break;
                case SDLK_c:
                    stream_cycle_channel(AVMEDIA_TYPE_VIDEO);
                    stream_cycle_channel(AVMEDIA_TYPE_AUDIO);
                    stream_cycle_channel(AVMEDIA_TYPE_SUBTITLE);
                    break;
                case SDLK_t:
                    stream_cycle_channel(AVMEDIA_TYPE_SUBTITLE);
                    break;
                case SDLK_w:
#if CONFIG_AVFILTER
                    if (this->show_mode == SHOW_MODE_VIDEO && this->vfilter_idx < nb_vfilters - 1) {
                        if (++this->vfilter_idx >= nb_vfilters)
                            this->vfilter_idx = 0;
                    }
                    else {
                        this->vfilter_idx = 0;
                        toggle_audio_display(cur_stream);
                    }
#else
                    toggle_audio_display();
#endif
                    break;
                case SDLK_PAGEUP:
                    if (this->ic->nb_chapters <= 1) {
                        incr = 600.0;
                        goto do_seek;
                    }
                    seek_chapter(1);
                    break;
                case SDLK_PAGEDOWN:
                    if (this->ic->nb_chapters <= 1) {
                        incr = -600.0;
                        goto do_seek;
                    }
                    seek_chapter(-1);
                    break;
                case SDLK_LEFT:
                    incr = seek_interval ? -seek_interval : -10.0;
                    goto do_seek;
                case SDLK_RIGHT:
                    incr = seek_interval ? seek_interval : 10.0;
                    goto do_seek;
                case SDLK_UP:
                    incr = 60.0;
                    goto do_seek;
                case SDLK_DOWN:
                    incr = -60.0;
                do_seek:
                    if (seek_by_bytes) {
                        pos = -1;
                        if (pos < 0 && this->video_stream >= 0)
                            pos = this->pictq.LastPos();
                        if (pos < 0 && this->audio_stream >= 0)
                            pos = this->sampq.LastPos();
                        if (pos < 0)
                            pos = avio_tell(this->ic->pb);
                        if (this->ic->bit_rate)
                            incr *= this->ic->bit_rate / 8.0;
                        else
                            incr *= 180000.0;
                        pos += incr;
                        stream_seek(pos, incr, 1);
                    }
                    else {
                        pos = get_master_clock();
                        if (isnan(pos))
                            pos = (double)this->seek_pos / AV_TIME_BASE;
                        pos += incr;
                        if (this->ic->start_time != AV_NOPTS_VALUE && pos < this->ic->start_time / (double)AV_TIME_BASE)
                            pos = this->ic->start_time / (double)AV_TIME_BASE;
                        stream_seek((int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE), 0);
                    }
                    break;
                default:
                    break;
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (exit_on_mousedown) {
                    do_exit();
                    break;
                }
                if (event.button.button == SDL_BUTTON_LEFT) {
                    static int64_t last_mouse_left_click = 0;
                    if (av_gettime_relative() - last_mouse_left_click <= 500000) {
                        toggle_full_screen();
                        this->force_refresh = 1;
                        last_mouse_left_click = 0;
                    }
                    else {
                        last_mouse_left_click = av_gettime_relative();
                    }
                }
            case SDL_MOUSEMOTION:
                if (cursor_hidden) {
                    SDL_ShowCursor(1);
                    cursor_hidden = 0;
                }
                cursor_last_shown = av_gettime_relative();
                if (event.type == SDL_MOUSEBUTTONDOWN) {
                    if (event.button.button != SDL_BUTTON_RIGHT)
                        break;
                    x = event.button.x;
                }
                else {
                    if (!(event.motion.state & SDL_BUTTON_RMASK))
                        break;
                    x = event.motion.x;
                }
                if (seek_by_bytes || this->ic->duration <= 0) {
                    uint64_t size = avio_size(this->ic->pb);
                    stream_seek(size * x / this->width, 0, 1);
                }
                else {
                    int64_t ts;
                    int ns, hh, mm, ss;
                    int tns, thh, tmm, tss;
                    tns = this->ic->duration / 1000000LL;
                    thh = tns / 3600;
                    tmm = (tns % 3600) / 60;
                    tss = (tns % 60);
                    frac = x / this->width;
                    ns = frac * tns;
                    hh = ns / 3600;
                    mm = (ns % 3600) / 60;
                    ss = (ns % 60);
                    av_log(nullptr, AV_LOG_INFO,
                        "Seek to %2.0f%% (%2d:%02d:%02d) of total duration (%2d:%02d:%02d)       \n", frac * 100,
                        hh, mm, ss, thh, tmm, tss);
                    ts = frac * this->ic->duration;
                    if (this->ic->start_time != AV_NOPTS_VALUE)
                        ts += this->ic->start_time;
                    stream_seek(ts, 0, 0);
                }
                break;
            case SDL_WINDOWEVENT:
                switch (event.window.event) {
                case SDL_WINDOWEVENT_SIZE_CHANGED:
                    screen_width = this->width = event.window.data1;
                    screen_height = this->height = event.window.data2;
                    if (this->vis_texture) {
                        SDL_DestroyTexture(this->vis_texture);
                        this->vis_texture = nullptr;
                    }
                case SDL_WINDOWEVENT_EXPOSED:
                    this->force_refresh = 1;
                }
                break;
            case SDL_QUIT:
            case FF_QUIT_EVENT:
                do_exit();
                break;
            default:
                break;
            }
        }
    }

public:
    static EShowMode eShow_mode;

private:
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_RendererInfo renderer_info = { 0 };
    SDL_AudioDeviceID audio_dev;

    std::future<int> future;
    AVInputFormat* iformat = nullptr;
    int abort_request;
    int force_refresh;
    int paused;
    int last_paused;
    int queue_attachments_req;
    int seek_req;
    int seek_flags;
    int loop = 1;
    bool framedrop = false;
    int infinite_buffer = -1;
    uint8_t startup_volume = 100;
    int64_t seek_pos;
    int64_t seek_rel;
    int read_pause_return;
    AVFormatContext* ic = nullptr;
    int realtime;

    /* current context */
    int is_full_screen;
    int64_t audio_callback_time;

    int64_t cursor_last_shown;
    int cursor_hidden = 0;

    int64_t start_time = AV_NOPTS_VALUE;
    int64_t duration = AV_NOPTS_VALUE;

    uint16_t default_width = 640;
    uint16_t default_height = 480;
    uint16_t screen_width = 0;
    uint16_t screen_height = 0;
    uint16_t screen_left = SDL_WINDOWPOS_CENTERED;
    uint16_t screen_top = SDL_WINDOWPOS_CENTERED;

    int seek_by_bytes = -1;
    float seek_interval = 10;

    double rdftspeed = 0.02;

    SClock audclk;
    SClock vidclk;
    SClock extclk;

    FrameQueue pictq;
    FrameQueue subpq;
    FrameQueue sampq;

    Decoder auddec;
    Decoder viddec;
    Decoder subdec;

    int audio_stream;

    ESyncType av_sync_type;

    double audio_clock;
    int audio_clock_serial;
    double audio_diff_cum; /* used for AV difference average computation */
    double audio_diff_avg_coef;
    double audio_diff_threshold;
    int audio_diff_avg_count;
    AVStream* audio_st = nullptr;
    PacketQueue audioq;
    int audio_hw_buf_size;
    uint8_t* audio_buf = nullptr;
    uint8_t* audio_buf1 = nullptr;
    unsigned int audio_buf_size; /* in bytes */
    unsigned int audio_buf1_size;
    int audio_buf_index; /* in bytes */
    int audio_write_buf_size;
    int audio_volume;
    int muted;
    struct AudioParams audio_src;
#if CONFIG_AVFILTER
    struct AudioParams audio_filter_src;
#endif
    struct AudioParams audio_tgt;
    struct SwrContext* swr_ctx;
    int frame_drops_early;
    int frame_drops_late;

    int16_t sample_array[SAMPLE_ARRAY_SIZE];
    int sample_array_index;
    int last_i_start;
    RDFTContext* rdft = nullptr;
    int rdft_bits;
    FFTSample* rdft_data = nullptr;
    int xpos;
    double last_vis_time;
    SDL_Texture* vis_texture = nullptr;
    SDL_Texture* sub_texture = nullptr;
    SDL_Texture* vid_texture = nullptr;

    int subtitle_stream;
    AVStream* subtitle_st = nullptr;
    PacketQueue subtitleq;

    double frame_timer;
    double frame_last_returned_time;
    double frame_last_filter_delay;
    int video_stream;
    AVStream* video_st = nullptr;
    PacketQueue videoq;
    double max_frame_duration;      // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity
    struct SwsContext* img_convert_ctx = nullptr;
    struct SwsContext* sub_convert_ctx = nullptr;
    int eof;

    char* filename = nullptr;
    int width, height, xleft, ytop;
    int step;

#if CONFIG_AVFILTER
    int vfilter_idx;
    AVFilterContext* in_video_filter;   // the first filter in the video chain
    AVFilterContext* out_video_filter;  // the last filter in the video chain
    AVFilterContext* in_audio_filter;   // the first filter in the audio chain
    AVFilterContext* out_audio_filter;  // the last filter in the audio chain
    AVFilterGraph* agraph;              // audio filter graph
#endif

    int last_video_stream, last_audio_stream, last_subtitle_stream;

    //SDL_cond* continue_read_thread;
    std::shared_ptr<std::condition_variable> continue_read_thread = nullptr;
};

static FMediaPlayer::ESyncType av_sync_type = FMediaPlayer::ESyncType::AV_SYNC_AUDIO_MASTER;

FMediaPlayer::EShowMode FMediaPlayer::eShow_mode = FMediaPlayer::EShowMode::SHOW_MODE_VIDEO;

#if CONFIG_AVFILTER
static int opt_add_vfilter(void* optctx, const char* opt, const char* arg)
{
    GROW_ARRAY(vfilters_list, nb_vfilters);
    vfilters_list[nb_vfilters - 1] = arg;
    return 0;
}
#endif

static inline
int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1,
    enum AVSampleFormat fmt2, int64_t channel_count2)
{
    /* If channel count == 1, planar and non-planar formats are the same */
    if (channel_count1 == 1 && channel_count2 == 1)
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    else
        return channel_count1 != channel_count2 || fmt1 != fmt2;
}

static inline
int64_t get_valid_channel_layout(int64_t channel_layout, int channels)
{
    if (channel_layout && av_get_channel_layout_nb_channels(channel_layout) == channels)
        return channel_layout;
    else
        return 0;
}


static int opt_frame_size(void* optctx, const char* opt, const char* arg)
{
    av_log(nullptr, AV_LOG_WARNING, "Option -s is deprecated, use -video_size.\n");
    return opt_default(nullptr, "video_size", arg);
}

static int opt_format(void* optctx, const char* opt, const char* arg)
{
    file_iformat = av_find_input_format(arg);
    if (!file_iformat) {
        av_log(nullptr, AV_LOG_FATAL, "Unknown input format: %s\n", arg);
        return AVERROR(EINVAL);
    }
    return 0;
}

static int opt_frame_pix_fmt(void* optctx, const char* opt, const char* arg)
{
    av_log(nullptr, AV_LOG_WARNING, "Option -pix_fmt is deprecated, use -pixel_format.\n");
    return opt_default(nullptr, "pixel_format", arg);
}

//static int opt_sync(void* optctx, const char* opt, const char* arg)
//{
//    if (!strcmp(arg, "audio"))
//        av_sync_type = AV_SYNC_AUDIO_MASTER;
//    else if (!strcmp(arg, "video"))
//        av_sync_type = AV_SYNC_VIDEO_MASTER;
//    else if (!strcmp(arg, "ext"))
//        av_sync_type = AV_SYNC_EXTERNAL_CLOCK;
//    else {
//        av_log(nullptr, AV_LOG_ERROR, "Unknown value for %s: %s\n", opt, arg);
//        exit(1);
//    }
//    return 0;
//}

static int opt_show_mode(void* optctx, const char* opt, const char* arg)
{
    FMediaPlayer::eShow_mode = !strcmp(arg, "video") ? FMediaPlayer::EShowMode::SHOW_MODE_VIDEO :
        !strcmp(arg, "waves") ? FMediaPlayer::EShowMode::SHOW_MODE_WAVES :
        !strcmp(arg, "rdft") ? FMediaPlayer::EShowMode::SHOW_MODE_RDFT : FMediaPlayer::EShowMode(int(parse_number_or_die(opt, arg, OPT_INT, 0, double(int(FMediaPlayer::EShowMode::SHOW_MODE_NB) - 1))));
    return 0;
}

static void opt_input_file(void* optctx, const char* filename)
{
    if (input_filename) {
        av_log(nullptr, AV_LOG_FATAL,
            "Argument '%s' provided as input filename, but '%s' was already specified.\n",
            filename, input_filename);
        exit(1);
    }
    if (!strcmp(filename, "-"))
        filename = "pipe:";
    input_filename = filename;
}

static int dummy;

static const OptionDef options[] = {
    //CMDUTILS_COMMON_OPTIONS
    //{ "x", HAS_ARG, /*{.func_arg = */opt_width/* }*/, "force displayed width", "width" },
    //{ "y", HAS_ARG, /*{.func_arg = */opt_height/* }*/, "force displayed height", "height" },
    { "s", HAS_ARG | OPT_VIDEO,/* {.func_arg = */opt_frame_size/* }*/, "set frame size (WxH or abbreviation)", "size" },
    //{ "fs", OPT_BOOL, { &is_full_screen }, "force full screen" },
    //{ "an", OPT_BOOL, { &audio_disable }, "disable audio" },
    //{ "vn", OPT_BOOL, { &video_disable }, "disable video" },
    //{ "sn", OPT_BOOL, { &subtitle_disable }, "disable subtitling" },
    { "ast", OPT_STRING | HAS_ARG | OPT_EXPERT, { &wanted_stream_spec[AVMEDIA_TYPE_AUDIO] }, "select desired audio stream", "stream_specifier" },
    { "vst", OPT_STRING | HAS_ARG | OPT_EXPERT, { &wanted_stream_spec[AVMEDIA_TYPE_VIDEO] }, "select desired video stream", "stream_specifier" },
    { "sst", OPT_STRING | HAS_ARG | OPT_EXPERT, { &wanted_stream_spec[AVMEDIA_TYPE_SUBTITLE] }, "select desired subtitle stream", "stream_specifier" },
    //{ "ss", HAS_ARG, /*{.func_arg = */opt_seek/* }*/, "seek to a given position in seconds", "pos" },
    //{ "t", HAS_ARG, /*{.func_arg = */opt_duration/* }*/, "play  \"duration\" seconds of audio/video", "duration" },
    //{ "bytes", OPT_INT | HAS_ARG, { &seek_by_bytes }, "seek by bytes 0=off 1=on -1=auto", "val" },
    //{ "seek_interval", OPT_FLOAT | HAS_ARG, { &seek_interval }, "set seek interval for left/right keys, in seconds", "seconds" },
    //{ "nodisp", OPT_BOOL, { &display_disable }, "disable graphical display" },
    //{ "noborder", OPT_BOOL, { &borderless }, "borderless window" },
    { "alwaysontop", OPT_BOOL, { &alwaysontop }, "window always on top" },
    //{ "volume", OPT_INT | HAS_ARG, { &startup_volume}, "set startup volume 0=min 100=max", "volume" },
    { "f", HAS_ARG,/* {.func_arg = */opt_format/* }*/, "force format", "fmt" },
    { "pix_fmt", HAS_ARG | OPT_EXPERT | OPT_VIDEO, /*{.func_arg = */opt_frame_pix_fmt/* }*/, "set pixel format", "format" },
    { "stats", OPT_BOOL | OPT_EXPERT, { &show_status }, "show status", "" },
    //{ "fast", OPT_BOOL | OPT_EXPERT, { &fast }, "non spec compliant optimizations", "" },
    //{ "genpts", OPT_BOOL | OPT_EXPERT, { &genpts }, "generate pts", "" },
    { "drp", OPT_INT | HAS_ARG | OPT_EXPERT, { &decoder_reorder_pts }, "let decoder reorder pts 0=off 1=on -1=auto", ""},
    { "lowres", OPT_INT | HAS_ARG | OPT_EXPERT, { &lowres }, "", "" },
    //{ "sync", HAS_ARG | OPT_EXPERT,/* {.func_arg = */opt_sync/* }*/, "set audio-video sync. type (type=audio/video/ext)", "type" },
    { "autoexit", OPT_BOOL | OPT_EXPERT, { &autoexit }, "exit at the end", "" },
    { "exitonkeydown", OPT_BOOL | OPT_EXPERT, { &exit_on_keydown }, "exit on key down", "" },
    { "exitonmousedown", OPT_BOOL | OPT_EXPERT, { &exit_on_mousedown }, "exit on mouse down", "" },
    //{ "loop", OPT_INT | HAS_ARG | OPT_EXPERT, { &loop }, "set number of times the playback shall be looped", "loop count" },
    //{ "framedrop", OPT_BOOL | OPT_EXPERT, { &framedrop }, "drop frames when cpu is too slow", "" },
    //{ "infbuf", OPT_BOOL | OPT_EXPERT, { &infinite_buffer }, "don't limit the input buffer size (useful with realtime streams)", "" },
    //{ "window_title", OPT_STRING | HAS_ARG, { &window_title }, "set window title", "window title" },
    //{ "left", OPT_INT | HAS_ARG | OPT_EXPERT, { &screen_left }, "set the x position for the left of the window", "x pos" },
    //{ "top", OPT_INT | HAS_ARG | OPT_EXPERT, { &screen_top }, "set the y position for the top of the window", "y pos" },
#if CONFIG_AVFILTER
    { "vf", OPT_EXPERT | HAS_ARG, {.func_arg = opt_add_vfilter }, "set video filters", "filter_graph" },
    { "af", OPT_STRING | HAS_ARG, { &afilters }, "set audio filters", "filter_graph" },
#endif
    //{ "rdftspeed", OPT_INT | HAS_ARG | OPT_AUDIO | OPT_EXPERT, { &rdftspeed }, "rdft speed", "msecs" },
    { "showmode", HAS_ARG, /*{.func_arg = */opt_show_mode/*}*/, "select show mode (0 = video, 1 = waves, 2 = RDFT)", "mode" },
    { "default", HAS_ARG | OPT_AUDIO | OPT_VIDEO | OPT_EXPERT, /*{.func_arg = */opt_default/* }*/, "generic catch all option", "" },
    { "i", OPT_BOOL, { &dummy}, "read specified file", "input_file"},
    //{ "codec", HAS_ARG,/* {.func_arg = */opt_codec/*}*/, "force decoder", "decoder_name" },
    //{ "acodec", HAS_ARG | OPT_STRING | OPT_EXPERT, {    &audio_codec_name }, "force audio decoder",    "decoder_name" },
    //{ "scodec", HAS_ARG | OPT_STRING | OPT_EXPERT, { &subtitle_codec_name }, "force subtitle decoder", "decoder_name" },
   // { "vcodec", HAS_ARG | OPT_STRING | OPT_EXPERT, {    &video_codec_name }, "force video decoder",    "decoder_name" },
    { "autorotate", OPT_BOOL, { &autorotate }, "automatically rotate video", "" },
    { "find_stream_info", OPT_BOOL | OPT_INPUT | OPT_EXPERT, { &find_stream_info },
        "read and decode the streams to fill missing information with heuristics" },
    { "filter_threads", HAS_ARG | OPT_INT | OPT_EXPERT, { &filter_nbthreads }, "number of filter threads per graph" },
    { nullptr, },
};

static void show_usage(void)
{
    av_log(nullptr, AV_LOG_INFO, "Simple media player\n");
    av_log(nullptr, AV_LOG_INFO, "usage: %s [options] input_file\n", program_name);
    av_log(nullptr, AV_LOG_INFO, "\n");
}

void show_help_default(const char* opt, const char* arg)
{
    av_log_set_callback(log_callback_help);
    show_usage();
    show_help_options(options, "Main options:", 0, OPT_EXPERT, 0);
    show_help_options(options, "Advanced options:", OPT_EXPERT, 0, 0);
    printf("\n");
    show_help_children(avcodec_get_class(), AV_OPT_FLAG_DECODING_PARAM);
    show_help_children(avformat_get_class(), AV_OPT_FLAG_DECODING_PARAM);
#if !CONFIG_AVFILTER
    show_help_children(sws_get_class(), AV_OPT_FLAG_ENCODING_PARAM);
#else
    show_help_children(avfilter_get_class(), AV_OPT_FLAG_FILTERING_PARAM);
#endif
    printf("\nWhile playing:\n"
        "q, ESC              quit\n"
        "f                   toggle full screen\n"
        "p, SPC              pause\n"
        "m                   toggle mute\n"
        "9, 0                decrease and increase volume respectively\n"
        "/, *                decrease and increase volume respectively\n"
        "a                   cycle audio channel in the current program\n"
        "v                   cycle video channel\n"
        "t                   cycle subtitle channel in the current program\n"
        "c                   cycle program\n"
        "w                   cycle video filters or show modes\n"
        "s                   activate frame-step mode\n"
        "left/right          seek backward/forward 10 seconds or to custom interval if -seek_interval is set\n"
        "down/up             seek backward/forward 1 minute\n"
        "page down/page up   seek backward/forward 10 minutes\n"
        "right mouse click   seek to percentage in file corresponding to fraction of width\n"
        "left double-click   toggle full screen\n"
    );
}

/* Called from the main */
int main(int argc, char** argv)
{
    int flags;
    init_dynload();

    parse_loglevel(argc, argv, options);

    FMediaPlayer::InitContext();

    std::unique_ptr<FMediaPlayer> pPlayer = std::make_unique<FMediaPlayer>();
    if (pPlayer == nullptr)
    {
        av_log(nullptr, AV_LOG_FATAL, "Failed to Create FMediaPlayer\n");
        return -1;
    }

    init_opts();

    signal(SIGINT, FMediaPlayer::sigterm_handler); /* Interrupt (ANSI).    */
    signal(SIGTERM, FMediaPlayer::sigterm_handler); /* Termination (ANSI).  */

    show_banner(argc, argv, options);

    parse_options(nullptr, argc, argv, options, opt_input_file);

    if (!input_filename) 
    {
        show_usage();
        av_log(nullptr, AV_LOG_FATAL, "An input file must be specified\n");
        av_log(nullptr, AV_LOG_FATAL, "Use -h to get full help or, even better, run 'man %s'\n", program_name);
        exit(1);
    }

    pPlayer->InitRender();

    if (!pPlayer->stream_open(input_filename, file_iformat))
    {
        av_log(nullptr, AV_LOG_FATAL, "Failed to Open stream\n");
        return -1;
    }
    
    pPlayer->event_loop();

    /* never returns */

    return 0;
}
