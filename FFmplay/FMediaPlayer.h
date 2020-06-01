#pragma once

#include <string>
#include <atomic>

#include "FDecoder.h"

extern "C"
{
#include "config.h"
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>

#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavcodec/avfft.h"
#include "libavdevice/avdevice.h"
#include "libswresample/swresample.h"

#if CONFIG_AVFILTER
# include "libavfilter/avfilter.h"
# include "libavfilter/buffersink.h"
# include "libavfilter/buffersrc.h"
#endif

#include <SDL.h>

//#include "cmdutils.h"
}

/* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
/* TODO: We assume that a decoded and resampled frame fits into this buffer */
constexpr uint32_t SAMPLE_ARRAY_SIZE = (8 * 65536);

struct AudioParams
{
    int freq;
    int channels;
    int64_t channel_layout;
    AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
};

/*
* \@brief ����ý���ͬ����
*/
class SClock
{
    friend class FMediaPlayer;
public:
    void Init(int* queue_serial);

    double Get();

    void SetAt(double pts, int serial, double time);

    void Set(double pts, int serial);

    /*
    * \@brief ����ʱ���ٶ�
    * \@param speed[in]:
    */
    void SetSpeed(double speed);

private:
    double pts;           /* clock base */
    double pts_drift;     /* clock base minus time at which we updated the clock */
    double last_updated;
    double speed = 1.0;
    int serial;           /* clock is based on a packet with this serial */
    bool paused = false;
    int* queue_serial = nullptr;    /* pointer to the current packet queue serial, used for obsolete clock detection */
};


/*
* \@brief ý�岥�Žӿ�
*/
class FMediaPlayer
{
public:
    /* ��ʾ��ʽ */
    enum struct EShowMode : uint8_t
    {
        SHOW_MODE_NONE = 0, 
        SHOW_MODE_VIDEO, /* ��ʾ����Ƶ */
        SHOW_MODE_WAVES, /* ��ʾ���Ʋ��� */
        SHOW_MODE_RDFT,  /* @TODO */
        SHOW_MODE_NB     /* Ч����ͬ��SHOW_MODE_RDFT */
    };

    /* ����ͬ����ʽ */
    enum struct ESyncType : uint8_t
    {
        AV_SYNC_AUDIO_MASTER,   /* ��ƵΪ��׼ͬ�� */
        AV_SYNC_VIDEO_MASTER,   /* ��ƵΪ��׼ͬ�� */
        AV_SYNC_EXTERNAL_CLOCK, /* �ⲿʱ��ͬ�� */
    };

public:
    explicit FMediaPlayer();
    ~FMediaPlayer();

    FMediaPlayer(const FMediaPlayer&) = delete;
    FMediaPlayer& operator=(const FMediaPlayer&) = delete;

private:
    /* ��ʵ���������� �����Զ����������Դ*/
    static uint8_t g_nInstance;

    /*
    * \@brief ��ʼ��FFmpeg��SDL
    */
    static bool initContext();

    /*
    * \@brief ����ʼ������
    */
    static void unInitContext();

    /*
    * \@brief ��ʼ����Ⱦ���
    */
    bool initRender();

    /*
    * \@brief ����ʼ����Ⱦ���
    */
    void uninitRender();
public:
    /*
    * \@brief ��ý����
    * \@param sURL:ý�����Ĵ��λ�ã�������ֱ���ĵ�ַ��Ҳ���Ǳ��ص�����ͷ
    * \@param iformat
    * \@return true::�򿪳ɹ� false::ʧ��
    */
    bool StreamOpen(const std::string& sURL, AVInputFormat* iformat = nullptr);

    /*
    * \@brief �ر�ý����
    */
    void StreamClose();

    /*
    * \@brief handle an event sent by the GUI
    */
    void EventLoop();

    /*
   * \@brief
   */
    void OnToggleFullScreen();

    /*
    * \@brief
    */
    void OnToggleAudioDisplay();

    /*
    * \@brief
    */
    void OnTogglePause();

    /*
    * \@brief
    */
    void OnToggleMute();

    /*
    * \@brief ��֡����
    */
    void OnStepToNextFrame();

    /*
    * \@brief @TODO seek in the stream 
    * \@param pos
    * \@param rel
    * \@param seek_by_bytes
    */
    void OnStreamSeek(int64_t pos, int64_t rel, bool seek_by_bytes);

    /*
    * \@brief ���ڲ��ŵ�����
    * \@param sign: ��С����
    * \@param step: ������С
    */
    void OnUpdateVolume(int sign, double step);

    /*
    * \@brief �˳�����
    */
    void OnExit();

    /*
    * \@brief @TODO
    */
    static void sigterm_handler(int sig);

private:
    /*
    * \@brief ���ô��ڵ�Ĭ�ϴ�С
    * \@param width[in] ���ڿ��
    * \@param height[in] ���ڸ߶�
    * \@param sar[in]
    */
    void set_default_window_size(int width, int height, AVRational sar);

    /*
    * \@brief �����Σ�������ʾ���Ʋ���
    * \@param nXPos[in]��
    * \@param nYPos[in]��
    * \@param nWidth[in]�����ο��
    * \@param nHeight[in]�����θ߶�
    */
    inline void fill_rectangle(int nXPos, int nYPos, int nWidth, int nHeight);

    /*
    * \@brief @TODO
    * \@param texture
    * \@param nNewFormat[in]
    * \@param nNewWidth[in]
    * \@param nNewHeight[in]
    * \@param eBlendmode[in]
    * \@param bInitTexture[in]
    * \@retrun 
    */
    bool realloc_texture(SDL_Texture** texture, Uint32 nNewFormat, int nNewWidth, int nNewHeight, SDL_BlendMode eBlendmode, bool bInitTexture);

    /*
    * \@brief 
    * \@param rect[in,out]
    * \@param srcRect[in]
    * \@param pic_width[in]
    * \@param pic_height[in]
    * \@param pic_sar[in]
    */
    void calculate_display_rect(SDL_Rect& rect, const SDL_Rect& srcRect, int pic_width, int pic_height, AVRational pic_sar);

    /*
    * \@brief ��ȡSDL ��Ⱦ��ģʽ
    * \@param nFormat[in]
    * \@param sdl_pix_fmt[in,out]
    * \@param sdl_blendmode[in,out]
    */
    void get_sdl_pix_fmt_and_blendmode(int nFormat, Uint32* sdl_pix_fmt, SDL_BlendMode* sdl_blendmode);

    /*
    * \@brief ����SDL-YUVת����ģʽ
    * \@param frame[in]:ԭʼͼ������
    */
    void set_sdl_yuv_conversion_mode(AVFrame* frame);

    /*
    * \@brief ���ⲿ�����ԭʼͼ��������������������
    * \@param tex[in,out]
    * \@param frame[in]:�����ԭʼ��Ƶ����
    * \@param img_convert_ctx[in,out]
    * \@retrun true:successed false:failured
    */
    bool upload_texture(SDL_Texture** tex, AVFrame* frame, struct SwsContext** img_convert_ctx);

    /* 
    * \@breif ��ʾ��ǰͼƬ�������) 
    */
    void video_display();

    /*
    * \@brief ����ͼ�����Ļ����
    */
    void video_image_display();

    /*
    * \@brief ������Ƶ��������
    */
    void video_audio_display();

    /*
    * \@brief ȡ������
    */
    static inline int compute_mod(int a, int b);

    /*
    * \@brief �ر�ָ��ͨ����ý����
    * \@param nStreamIndex[in] ý��ͨ������
    */
    void stream_component_close(int nStreamIndex);

    /*
    * \@brief ����ý�����������Ϣ @TODO
    * \@param s[in]
    * \@param codec_opts[in]
    * \@return @TODO
    */
    AVDictionary** setup_find_stream_info_opts(AVFormatContext* s, AVDictionary* codec_opts);

    /*
    * \@brief @TODO ͬ��ʱ��
    * \@param c[in]
    * \@param slave[in]
    */
    static void sync_clock_to_slave(SClock& c, SClock& slave);

    /*
    * \@brief ��ȡͬ������
    */
    ESyncType get_master_sync_type();

    /* 
    * \@brief ��ȡ��ǰ��ʱ��ֵ 
    */
    double get_master_clock();

    /*
    * \brief @TODO
    */
    void check_external_clock_speed();

    /* 
    * \@brief pause or resume the video 
    */
    void stream_toggle_pause();

    /*
    * \@brief �����ӳ��ж��
    * \@param delay[in] ��һ֡�ĳ���ʱ��
    * \@return �ӳ�ʱ��
    */
    double compute_target_delay(double delay);

    /*
    * \@breif �����ٽ�֡�ĳ���ʱ��
    * \@param vp[in] 
    * \@param nextvp[in]
    * \@return 
    */
    double vp_duration(SFrame* vp, SFrame* nextvp);

    /*
    * \@brief ������Ƶ֡��ʱ���
    * \@param pts[in]
    * \@param pos[in]
    * \@param serial[in]
    */
    void update_video_pts(double pts, int64_t pos, int serial);

    /* 
    * \@brief called to display each frame 
    * \@param remaining_time[in,out]
    */
    void video_refresh(double& remaining_time);
    
    int queue_picture(AVFrame* src_frame, double pts, double duration, int64_t pos, int serial);

    int get_video_frame(AVFrame* frame);

#if CONFIG_AVFILTER
    static int configure_filtergraph(AVFilterGraph* graph, const char* filtergraph,
        AVFilterContext* source_ctx, AVFilterContext* sink_ctx);

    int configure_video_filters(AVFilterGraph* graph, const char* vfilters, AVFrame* frame);

    int configure_audio_filters(const char* afilters, int force_output_format);
#endif  /* CONFIG_AVFILTER */

    /*
    * \@brief ���ڻ�ȡ��Ƶ����
    * \@return
    */
    int audio_thread();

    /*
    * \@brief ���ڻ�ȡ��Ƶ����
    * \@return
    */
    int video_thread();

    /*
    * \@brief ���ڻ�ȡ��Ļ����
    * \@return
    */
    int subtitle_thread();

    /* 
    * \@brief ���̴߳Ӵ��̻������ȡ��
    * \@return
    */
    int read_thread();

    int video_open();

    int audio_open(int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams* audio_hw_params);

    /* prepare a new audio buffer */
    static void sdl_audio_callback(void* pUserData, Uint8* stream, int len);

    static int decode_interrupt_cb(void* pUserData);

    /* copy samples for viewing in editor window */
    void update_sample_display(short* samples, int samples_size);

    /* return the wanted number of samples to get better sync if sync_type is video
     * or external master clock */
    int synchronize_audio(int nb_samples);

    /**
     * Decode one audio frame and return its uncompressed size.
     *
     * The processed audio frame is decoded, converted if required, and
     * stored in is->audio_buf, with size in bytes given by the return
     * value.
     */
    int audio_decode_frame();

    /* open a given stream. Return 0 if OK */
    int stream_component_open(int stream_index);

    bool stream_has_enough_packets(AVStream* st, int stream_id, PacketQueue* queue);

    /*
    * \@brief ���ŵ�ý���Ƿ���ʵʱ�ģ���Ҫ�жϸ���Э�����ж�
    */
    bool is_realtime();

    void stream_cycle_channel(int codec_type);

    void refresh_loop_wait_event(SDL_Event* event);

    void seek_chapter(int incr);

public:
    static EShowMode eShow_mode;

private:
    SDL_Window* pWindow = nullptr;
    SDL_Renderer* pRenderer = nullptr;
    SDL_RendererInfo renderer_info = { 0 };
    SDL_AudioDeviceID audio_dev;

    /* @TODO */
    AVPacket flush_pkt;
    /* ���Ի�ȡ�߳�ִ�еĽ�����Լ��߳�ͬ�� */
    std::future<int> future;
    /*  */
    AVInputFormat* pInputformat = nullptr;
    bool abort_request = false;
    int force_refresh;
    bool paused = false;
    bool last_paused = false;
    int queue_attachments_req;
    bool seek_req = false;
    int seek_flags;
    int loop = 1;
    bool framedrop = false;
    /* ���޻����� */
    bool infinite_buffer = false;
    uint8_t startup_volume = 100;
    int64_t seek_pos;
    int64_t seek_rel;
    int read_pause_return;
    /*  */
    AVFormatContext* pFormatCtx = nullptr;

    /* �Ƿ��Զ��˳� */
    bool autoexit = false;

    int alwaysontop = false;

    const char* wanted_stream_spec[AVMEDIA_TYPE_NB] = { 0 };

    unsigned sws_flags = SWS_BICUBIC;

    int decoder_reorder_pts = -1;

    int lowres = 0;

#if CONFIG_AVFILTER
    const char** vfilters_list = nullptr;
    int nb_vfilters = 0;
    char* afilters = nullptr;
#endif
    int autorotate = 1;
    /* number of filter threads per graph */
    int filter_nbthreads = 0;

    /* current context */
    bool is_full_screen = false;
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

    bool seek_by_bytes = false;
    float seek_interval = 10;

    double rdftspeed = 0.02;

    SClock audclk;  /* ��Ƶʱ�� */
    SClock vidclk;  /* ��Ƶʱ�� */
    SClock extclk;  /* �ⲿʱ�� */

    FrameQueue pictq;
    FrameQueue subpq;
    FrameQueue sampq;

    FDecoder auddec;
    FDecoder viddec;
    FDecoder subdec;

    int audio_stream = -1;

    ESyncType av_sync_type;

    double audio_clock;
    int audio_clock_serial;
    double audio_diff_cum; /* used for AV difference average computation */
    double audio_diff_avg_coef;
    double audio_diff_threshold;
    int audio_diff_avg_count;
    AVStream* pAudioStream = nullptr;
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
    struct SwrContext* swr_ctx = nullptr;
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
    /* ��Ļ���� */
    SDL_Texture* sub_texture = nullptr;
    /* ͼ������ */
    SDL_Texture* vid_texture = nullptr;

    int subtitle_stream = -1;
    AVStream* pSubtitleStream = nullptr;
    PacketQueue subtitleq;

    double frame_timer;
    double frame_last_returned_time;
    double frame_last_filter_delay;
    int video_stream = -1;
    AVStream* pVideoStream = nullptr;
    PacketQueue videoq;
    double max_frame_duration;      // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity
    struct SwsContext* img_convert_ctx = nullptr;
    /* ��Ļ��Ϣ */
    struct SwsContext* sub_convert_ctx = nullptr;
    int eof;

    char* sURL = nullptr;
    //int width, height, xleft, ytop;
    SDL_Rect rect{};
    int step;

#if CONFIG_AVFILTER
    int vfilter_idx;
    AVFilterContext* in_video_filter = nullptr;   // the first filter in the video chain
    AVFilterContext* out_video_filter = nullptr;  // the last filter in the video chain
    AVFilterContext* in_audio_filter = nullptr;   // the first filter in the audio chain
    AVFilterContext* out_audio_filter = nullptr;  // the last filter in the audio chain
    AVFilterGraph* agraph = nullptr;              // audio filter graph
#endif

    int last_video_stream = -1, last_audio_stream = -1, last_subtitle_stream = -1;

    std::shared_ptr<std::condition_variable> continue_read_thread = nullptr;
};