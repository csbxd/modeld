#include "modeld/media.hpp"
#include "modeld/common.hpp"
#include <cuda.h>
#include <cuda_runtime_api.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#include <algorithm>
#include <cmath>
#include <deque>
#include <iostream>
#include <limits>
namespace modeld {
namespace {
void avcheck(int value, const char *operation) {
    if (value < 0) {
        char text[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(value, text, sizeof text);
        throw Error(502, std::string(operation) + ": " + text);
    }
}
struct SourceError : Error {
    using Error::Error;
};
void sourcecheck(int value, const char *operation) {
    try {
        avcheck(value, operation);
    } catch (const Error &e) {
        throw SourceError(e.status, e.what());
    }
}
struct FrameDelete {
    void operator()(AVFrame *f) const {
        av_frame_free(&f);
    }
};
using Frame = std::unique_ptr<AVFrame, FrameDelete>;
Frame new_frame() {
    auto *f = av_frame_alloc();
    if (!f)
        throw std::bad_alloc();
    return Frame(f);
}
struct Packet {
    AVPacket *p = av_packet_alloc();
    ~Packet() {
        av_packet_free(&p);
    }
};
struct Mux {
    AVFormatContext *format = nullptr;
    AVIOContext *io = nullptr;
    std::vector<uint8_t> bytes;
    ~Mux() {
        if (format) {
            format->pb = nullptr;
            avformat_free_context(format);
        }
        if (io) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
    }
    static int write(void *opaque, const uint8_t *b, int n) {
        auto *m = static_cast<Mux *>(opaque);
        if (n < 0 || m->bytes.size() + static_cast<size_t>(n) > 64 * 1024 * 1024)
            return AVERROR(ENOSPC);
        try {
            m->bytes.insert(m->bytes.end(), b, b + n);
        } catch (...) {
            return AVERROR(ENOMEM);
        }
        return n;
    }
};
} // namespace
struct Video::Impl {
    AVFormatContext *input = nullptr;
    AVCodecContext *decoder = nullptr;
    AVCodecContext *encoder = nullptr;
    AVBufferRef *device = nullptr;
    AVBufferRef *frames = nullptr;
    SwsContext *sws = nullptr;
    int stream = -1, segment_seconds, qp;
    std::string encoder_name;
    std::vector<std::string> sources;
    std::string source_headers;
    size_t source_index = 0;
    AVCodecID source_codec = AV_CODEC_ID_NONE;
    bool source_ready = false;
    MediaInfo info{};
    double origin = 0;
    bool eof = false, flushed = false;
    int64_t next_segment = -1;
    RifeFactory factory;
    std::shared_ptr<Rife> rife;
    std::shared_ptr<std::atomic<bool>> cancelled;
    std::shared_ptr<Image> first, last;
    explicit Impl(int seconds, int quantizer, std::string codec, RifeFactory f,
                  std::shared_ptr<std::atomic<bool>> c)
        : segment_seconds(seconds), qp(quantizer), encoder_name(std::move(codec)),
          factory(std::move(f)), cancelled(std::move(c)) {}
    ~Impl() {
        avcodec_free_context(&encoder);
        av_buffer_unref(&frames);
        av_buffer_unref(&device);
        sws_freeContext(sws);
        avcodec_free_context(&decoder);
        avformat_close_input(&input);
    }
    void check() {
        if (cancelled->load())
            throw Error(410, "Interpolation session was closed");
    }
    static int interrupt(void *opaque) {
        return static_cast<Impl *>(opaque)->cancelled->load() ? 1 : 0;
    }
    std::shared_ptr<Image> read_frame() {
        auto frame = new_frame();
        Packet packet;
        for (;;) {
            check();
            int r = avcodec_receive_frame(decoder, frame.get());
            if (r == 0)
                break;
            if (r == AVERROR_EOF) {
                if (last && last->time + 1.0 < info.duration)
                    throw SourceError(502, "Source ended before its declared duration");
                return nullptr;
            }
            if (r != AVERROR(EAGAIN))
                sourcecheck(r, "Decode frame");
            if (eof) {
                if (flushed)
                    return nullptr;
                sourcecheck(avcodec_send_packet(decoder, nullptr), "Flush decoder");
                flushed = true;
                continue;
            }
            for (;;) {
                r = av_read_frame(input, packet.p);
                if (r == AVERROR_EOF) {
                    if (input->pb && input->pb->error < 0 && input->pb->error != AVERROR_EOF)
                        sourcecheck(input->pb->error, "Read source transport");
                    eof = true;
                    break;
                }
                sourcecheck(r, "Read source packet");
                if (packet.p->stream_index == stream) {
                    if (packet.p->flags & AV_PKT_FLAG_CORRUPT)
                        throw SourceError(502, "Incomplete source video packet");
                    sourcecheck(avcodec_send_packet(decoder, packet.p), "Send source packet");
                    av_packet_unref(packet.p);
                    break;
                }
                av_packet_unref(packet.p);
            }
        }
        if (frame->color_trc == AVCOL_TRC_SMPTE2084 || frame->color_trc == AVCOL_TRC_ARIB_STD_B67)
            throw Error(422, "HDR interpolation is not supported in this release; keep the "
                             "original video track");
        if (frame->width != info.width || frame->height != info.height)
            throw Error(422, "Variable-resolution media is not supported");
        auto out = std::make_shared<Image>();
        out->width = info.width;
        out->height = info.height;
        out->rgb.resize(static_cast<size_t>(info.width) * info.height * 3);
        auto timestamp = frame->best_effort_timestamp;
        out->time = timestamp == AV_NOPTS_VALUE
                        ? (last ? last->time + 1 / info.source_fps : 0)
                        : timestamp * av_q2d(input->streams[stream]->time_base) - origin;
        sws = sws_getCachedContext(
            sws, info.width, info.height, static_cast<AVPixelFormat>(frame->format), info.width,
            info.height, AV_PIX_FMT_RGB24, SWS_BICUBIC, nullptr, nullptr, nullptr);
        if (!sws)
            throw Error(500, "Cannot create color conversion context");
        int colors =
            frame->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709
            : frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M
                ? SWS_CS_ITU601
                : (info.width >= 1280 ? SWS_CS_ITU709 : SWS_CS_ITU601);
        const int *coefficients = sws_getCoefficients(colors);
        sws_setColorspaceDetails(sws, coefficients, frame->color_range == AVCOL_RANGE_JPEG,
                                 coefficients, 1, 0, 1 << 16, 1 << 16);
        uint8_t *destination[] = {out->rgb.data(), nullptr, nullptr, nullptr};
        int strides[] = {info.width * 3, 0, 0, 0};
        avcheck(sws_scale(sws, frame->data, frame->linesize, 0, info.height, destination, strides),
                "Convert source to RGB");
        return out;
    }
    void seek(double time) {
        auto *video = input->streams[stream];
        auto target =
            static_cast<int64_t>((std::max(0.0, time) + origin) / av_q2d(video->time_base));
        sourcecheck(av_seek_frame(input, stream, target, AVSEEK_FLAG_BACKWARD), "Seek source");
        avcodec_flush_buffers(decoder);
        eof = false;
        flushed = false;
        first = read_frame();
        last = read_frame();
        if (!first)
            throw SourceError(502, "Source seek produced no video frames");
    }
    void open_source(bool initialize) {
        check();
        source_ready = false;
        first.reset();
        last.reset();
        eof = flushed = false;
        next_segment = initialize ? -1 : -2;
        avcodec_free_context(&decoder);
        avformat_close_input(&input);
        input = avformat_alloc_context();
        if (!input)
            throw std::bad_alloc();
        input->interrupt_callback = {Impl::interrupt, this};
        AVDictionary *options = nullptr;
        av_dict_set(&options, "rw_timeout", "8000000", 0);
        av_dict_set(&options, "reconnect", "1", 0);
        av_dict_set(&options, "reconnect_on_network_error", "1", 0);
        av_dict_set(&options, "reconnect_on_http_error", "408,429,5xx", 0);
        av_dict_set(&options, "reconnect_max_retries", "2", 0);
        av_dict_set(&options, "reconnect_delay_max", "1", 0);
        av_dict_set(&options, "reconnect_delay_total_max", "2", 0);
        av_dict_set(&options, "protocol_whitelist", "http,https,tcp,tls,crypto", 0);
        av_dict_set(&options, "user_agent", "modeld/0.1", 0);
        if (!source_headers.empty())
            av_dict_set(&options, "headers", source_headers.c_str(), 0);
        int r = avformat_open_input(&input, sources[source_index].c_str(), nullptr, &options);
        av_dict_free(&options);
        sourcecheck(r, "Open source media");
        sourcecheck(avformat_find_stream_info(input, nullptr), "Probe source media");
        stream = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        sourcecheck(stream, "Find source video");
        auto *video = input->streams[stream];
        auto *p = video->codecpar;
        const double duration =
            input->duration != AV_NOPTS_VALUE ? input->duration / static_cast<double>(AV_TIME_BASE)
            : video->duration != AV_NOPTS_VALUE ? video->duration * av_q2d(video->time_base)
                                                : 0;
        const double fps = av_q2d(av_guess_frame_rate(input, video, nullptr));
        if (p->width < 32 || p->height < 32 || p->width > 1920 || p->height > 1080 ||
            p->width % 2 || p->height % 2)
            throw Error(422, "This release supports even native dimensions up to 1920x1080");
        if (!std::isfinite(duration) || duration <= 0 || duration > 604800)
            throw Error(422, "A seekable video with a known duration is required");
        if (!std::isfinite(fps) || fps <= 0 || fps >= 60)
            throw Error(422, "Source frame rate must be below 60 fps");
        if (initialize) {
            info = {p->width, p->height, duration, fps};
            source_codec = p->codec_id;
        } else if (p->width != info.width || p->height != info.height ||
                   p->codec_id != source_codec || std::abs(fps - info.source_fps) > 0.01 ||
                   std::abs(duration - info.duration) > 0.1) {
            throw Error(422, "Backup source does not match the selected video");
        }
        origin = input->start_time != AV_NOPTS_VALUE
                     ? input->start_time / static_cast<double>(AV_TIME_BASE)
                 : video->start_time != AV_NOPTS_VALUE
                     ? video->start_time * av_q2d(video->time_base)
                     : 0;
        // Native AV1 can be hardware-only; swscale requires software frames.
        const auto *codec = p->codec_id == AV_CODEC_ID_AV1
                                ? avcodec_find_decoder_by_name("libdav1d")
                                : avcodec_find_decoder(p->codec_id);
        if (!codec && p->codec_id == AV_CODEC_ID_AV1)
            throw Error(422, "AV1 sources require FFmpeg with the libdav1d software decoder");
        if (!codec)
            throw Error(422, "Source video decoder unavailable");
        decoder = avcodec_alloc_context3(codec);
        if (!decoder)
            throw std::bad_alloc();
        sourcecheck(avcodec_parameters_to_context(decoder, p), "Configure source decoder");
        decoder->thread_count = 4;
        sourcecheck(avcodec_open2(decoder, codec, nullptr), "Open source decoder");
        first = read_frame();
        last = read_frame();
        if (!first)
            throw SourceError(502, "Source contains no video frames");
        source_ready = true;
        std::cerr << "[modeld] Source decoder: " << codec->name << "; source " << source_index + 1
                  << '/' << sources.size() << '\n';
    }
    void prepare_encoder() {
        avcodec_free_context(&encoder);
        if (!device) {
            auto cuda_status = cudaSetDevice(0);
            if (cuda_status != cudaSuccess)
                throw Error(500, std::string("Activate inference CUDA context: ") +
                                     cudaGetErrorString(cuda_status));
            // Share TensorRT's active context without changing its scheduling flags
            // or transferring context ownership to FFmpeg.
            int r = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_CUDA, "0", nullptr,
                                           AV_CUDA_USE_CURRENT_CONTEXT);
            avcheck(r, "Create CUDA video device");
            frames = av_hwframe_ctx_alloc(device);
            if (!frames)
                throw std::bad_alloc();
            auto *f = reinterpret_cast<AVHWFramesContext *>(frames->data);
            f->format = AV_PIX_FMT_CUDA;
            f->sw_format = AV_PIX_FMT_NV12;
            f->width = info.width;
            f->height = info.height;
            f->initial_pool_size = 8;
            avcheck(av_hwframe_ctx_init(frames), "Create CUDA encoder frame pool");
        }
        auto *codec = avcodec_find_encoder_by_name(encoder_name.c_str());
        if (!codec)
            throw Error(500, "Required FFmpeg NVENC encoder is unavailable");
        encoder = avcodec_alloc_context3(codec);
        if (!encoder)
            throw std::bad_alloc();
        encoder->width = info.width;
        encoder->height = info.height;
        encoder->pix_fmt = AV_PIX_FMT_CUDA;
        encoder->time_base = {1, 60};
        encoder->framerate = {60, 1};
        encoder->gop_size = 60 * segment_seconds;
        encoder->max_b_frames = 0;
        encoder->thread_count = 1;
        encoder->hw_frames_ctx = av_buffer_ref(frames);
        encoder->color_primaries = AVCOL_PRI_BT709;
        encoder->color_trc = AVCOL_TRC_BT709;
        encoder->colorspace = AVCOL_SPC_BT709;
        encoder->color_range = AVCOL_RANGE_MPEG;
        encoder->sample_aspect_ratio = input->streams[stream]->sample_aspect_ratio;
        if (!encoder->sample_aspect_ratio.num)
            encoder->sample_aspect_ratio = {1, 1};
        AVDictionary *options = nullptr;
        av_dict_set(&options, "preset", "p4", 0);
        av_dict_set(&options, "tune", "ull", 0);
        av_dict_set(&options, "rc", "constqp", 0);
        av_dict_set_int(&options, "qp", qp, 0);
        av_dict_set(&options, "zerolatency", "1", 0);
        av_dict_set(&options, "forced-idr", "1", 0);
        av_dict_set(&options, "delay", "0", 0);
        av_dict_set(&options, "rc-lookahead", "0", 0);
        int r = avcodec_open2(encoder, codec, &options);
        av_dict_free(&options);
        avcheck(r, "Open NVENC encoder");
    }
};
Video::Video(const std::vector<std::string> &sources, const std::string &source_headers,
             int seconds, int qp, const std::string &encoder, RifeFactory factory,
             std::shared_ptr<std::atomic<bool>> cancelled)
    : impl(std::make_unique<Impl>(seconds, qp, encoder, std::move(factory), std::move(cancelled))) {
    if (sources.empty() || sources.size() > 9)
        throw Error(422, "One source and at most eight backup URLs are required");
    for (const auto &url : sources)
        if (!url.starts_with("http://") && !url.starts_with("https://"))
            throw Error(422, "source_url must be HTTP or HTTPS");
    auto &v = *impl;
    v.sources = sources;
    v.source_headers = source_headers;
    for (;;) {
        try {
            v.open_source(v.source_codec == AV_CODEC_ID_NONE);
            break;
        } catch (const SourceError &) {
            v.check();
            if (++v.source_index >= v.sources.size())
                throw;
            std::cerr << "[modeld] Initial source failed; trying backup " << v.source_index + 1
                      << '\n';
        }
    }
}
Video::~Video() = default;
MediaInfo Video::info() const {
    return impl->info;
}
Segment Video::render(int64_t sequence) {
    auto started = Clock::now();
    auto &v = *impl;
    v.check();
    int64_t begin = sequence * 60 * v.segment_seconds,
            total = static_cast<int64_t>(std::ceil(v.info.duration * 60)),
            end = std::min(begin + 60 * v.segment_seconds, total);
    if (sequence < 0 || begin >= total)
        throw Error(404, "Segment does not exist");
    auto render_once = [&]() -> Segment {
        if (!v.source_ready)
            v.open_source(false);
        if (v.next_segment != sequence) {
            if (sequence != 0 || v.next_segment != -1)
                v.seek(begin / 60.0);
            v.prepare_encoder();
        }
        if (!v.rife)
            v.rife = v.factory(v.info.width, v.info.height);
        Mux mux;
        avcheck(avformat_alloc_output_context2(&mux.format, nullptr, "mpegts", nullptr),
                "Create segment muxer");
        auto *out = avformat_new_stream(mux.format, nullptr);
        if (!out)
            throw std::bad_alloc();
        avcheck(avcodec_parameters_from_context(out->codecpar, v.encoder),
                "Configure segment stream");
        out->time_base = v.encoder->time_base;
        auto *buffer = static_cast<unsigned char *>(av_malloc(65536));
        if (!buffer)
            throw std::bad_alloc();
        mux.io = avio_alloc_context(buffer, 65536, 1, &mux, nullptr, Mux::write, nullptr);
        if (!mux.io) {
            av_free(buffer);
            throw std::bad_alloc();
        }
        mux.format->pb = mux.io;
        mux.format->flags |= AVFMT_FLAG_CUSTOM_IO;
        mux.format->max_delay = 0;
        mux.format->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
        AVDictionary *options = nullptr;
        av_dict_set(&options, "mpegts_copyts", "1", 0);
        av_dict_set(&options, "mpegts_flags", "resend_headers+initial_discontinuity", 0);
        int r = avformat_write_header(mux.format, &options);
        av_dict_free(&options);
        avcheck(r, "Write segment header");
        struct Pending {
            Frame frame;
            int slot;
        };
        std::deque<Pending> pending;
        struct PendingFence {
            std::deque<Pending> &pending;
            std::shared_ptr<Rife> rife;
            ~PendingFence() {
                for (auto &entry : pending) {
                    try {
                        rife->wait(entry.slot);
                    } catch (...) {
                    }
                }
            }
        } fence{pending, v.rife};
        int packets = 0;
        auto finish = [&] {
            auto p = std::move(pending.front());
            pending.pop_front();
            v.rife->wait(p.slot);
            avcheck(avcodec_send_frame(v.encoder, p.frame.get()), "Encode output frame");
            Packet packet;
            for (;;) {
                int n = avcodec_receive_packet(v.encoder, packet.p);
                if (n == AVERROR(EAGAIN) || n == AVERROR_EOF)
                    break;
                avcheck(n, "Receive encoded frame");
                av_packet_rescale_ts(packet.p, v.encoder->time_base, out->time_base);
                packet.p->stream_index = out->index;
                avcheck(av_interleaved_write_frame(mux.format, packet.p), "Mux encoded frame");
                packets++;
            }
        };
        for (int64_t index = begin; index < end; index++) {
            v.check();
            if (pending.size() == 2)
                finish();
            double time = index / 60.0;
            while (v.last && v.last->time < time - 1e-8) {
                v.first = v.last;
                v.last = v.read_frame();
            }
            auto last = v.last ? v.last : v.first;
            double interval = last->time - v.first->time;
            float t =
                interval > 1e-8
                    ? static_cast<float>(std::clamp((time - v.first->time) / interval, 0.0, 1.0))
                    : 0;
            auto frame = new_frame();
            avcheck(av_hwframe_get_buffer(v.frames, frame.get(), 0), "Allocate output GPU frame");
            frame->pts = index;
            frame->time_base = v.encoder->time_base;
            frame->color_range = AVCOL_RANGE_MPEG;
            frame->color_primaries = AVCOL_PRI_BT709;
            frame->color_trc = AVCOL_TRC_BT709;
            frame->colorspace = AVCOL_SPC_BT709;
            if (index == begin)
                frame->pict_type = AV_PICTURE_TYPE_I;
            int slot = static_cast<int>((index - begin) % 2);
            v.rife->enqueue(slot, *v.first, *last, t, frame->data[0], frame->data[1],
                            frame->linesize[0], frame->linesize[1]);
            pending.push_back({std::move(frame), slot});
        }
        while (!pending.empty())
            finish();
        if (packets != end - begin)
            throw Error(500, "NVENC output was delayed; zero-latency configuration is required");
        avcheck(av_write_trailer(mux.format), "Finish segment");
        avio_flush(mux.io);
        v.next_segment = sequence + 1;
        return {std::move(mux.bytes), static_cast<int>(end - begin),
                std::chrono::duration<double>(Clock::now() - started).count()};
    };
    for (int attempt = 0;; ++attempt) {
        try {
            return render_once();
        } catch (const SourceError &e) {
            v.check();
            v.source_ready = false;
            v.next_segment = -2;
            std::cerr << "[modeld] Source recovery: segment " << sequence << ", attempt "
                      << attempt + 1 << ": " << e.what() << '\n';
            v.source_index = (v.source_index + 1) % v.sources.size();
            if (attempt >= 1)
                throw;
        }
    }
}
} // namespace modeld
