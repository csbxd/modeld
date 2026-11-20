#include "modeld/media.hpp"
#include "modeld/core.hpp"
#include "modeld/fmp4.hpp"
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
#include <cstring>
#include <deque>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
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
using PacketRef = std::shared_ptr<AVPacket>;
PacketRef copy_packet(const AVPacket *packet) {
    auto *copy = av_packet_clone(packet);
    if (!copy)
        throw std::bad_alloc();
    return {copy, [](AVPacket *p) { av_packet_free(&p); }};
}
struct CopyTrack {
    AVCodecParameters *parameters = avcodec_parameters_alloc();
    AVDictionary *metadata = nullptr;
    AVRational time_base{};
    int source_index = -1, disposition = 0;
    bool separate = false;
    double origin = 0, latest_end = -1;
    std::deque<PacketRef> packets;
    ~CopyTrack() {
        avcodec_parameters_free(&parameters);
        av_dict_free(&metadata);
    }
};
struct Mux {
    AVFormatContext *format = nullptr;
    AVIOContext *io = nullptr;
    std::vector<uint8_t> bytes;
    std::vector<int64_t> first_dts;
    void packet(AVPacket *packet) {
        auto &first = first_dts.at(packet->stream_index);
        if (first == AV_NOPTS_VALUE)
            first = packet->dts;
        avcheck(av_interleaved_write_frame(format, packet), "Mux output packet");
    }
    // Each independently generated fragment has a new MOV context. Restore
    // absolute decode times, including the original AAC packet boundaries.
    void absolute_decode_times() {
        std::vector<std::optional<uint64_t>> origins(first_dts.size());
        auto be32 = [&](size_t p) -> uint32_t {
            if (p + 4 > bytes.size())
                throw Error(502, "Truncated MP4 box");
            return (uint32_t(bytes[p]) << 24) | (uint32_t(bytes[p + 1]) << 16) |
                   (uint32_t(bytes[p + 2]) << 8) | bytes[p + 3];
        };
        auto children = [&](size_t begin, size_t end, auto visit) {
            for (size_t p = begin; p < end;) {
                const auto length = be32(p);
                if (length < 8 || p + length > end)
                    throw Error(502, "Invalid MP4 fragment box");
                visit(p, p + length);
                p += length;
            }
        };
        children(0, bytes.size(), [&](size_t top, size_t top_end) {
            if (be32(top + 4) != 0x6d6f6f66)
                return; // moof
            children(top + 8, top_end, [&](size_t traf, size_t traf_end) {
                if (be32(traf + 4) != 0x74726166)
                    return;
                uint32_t track = 0;
                size_t decode = 0;
                children(traf + 8, traf_end, [&](size_t child, size_t) {
                    if (be32(child + 4) == 0x74666864)
                        track = be32(child + 12);
                    if (be32(child + 4) == 0x74666474)
                        decode = child;
                });
                if (!track || track > first_dts.size() || !decode)
                    throw Error(502, "Missing MP4 track timing");
                const auto base = first_dts[track - 1];
                if (base < 0)
                    throw Error(422, "Browser audio requires a nonnegative source timeline");
                const size_t count = bytes.at(decode + 8) == 1 ? 8 : 4;
                uint64_t value = 0;
                for (size_t i = 0; i < count; ++i)
                    value = (value << 8) | bytes.at(decode + 12 + i);
                auto &origin = origins[track - 1];
                if (!origin)
                    origin = value;
                value = value - *origin + static_cast<uint64_t>(base);
                for (size_t i = 0; i < count; ++i)
                    bytes.at(decode + 12 + count - 1 - i) = static_cast<uint8_t>(value >> (8 * i));
            });
        });
    }
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
struct MediaStream::Impl {
    bool fragmented_mp4 = false;
    AVFormatContext *input = nullptr;
    AVFormatContext *audio_input = nullptr;
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
    std::chrono::seconds source_timeout;
    Clock::time_point deadline{}, attempt_deadline{};
    const char *phase = "Source probe";
    std::shared_ptr<Image> first, last;
    Frame rgb_frame;
    TrackSelection selection;
    std::optional<Input> audio_source;
    size_t audio_index = 0;
    std::vector<std::unique_ptr<CopyTrack>> copy_tracks;
    std::deque<PacketRef> queued_video;
    std::vector<uint8_t> init_header;
    bool audio_eof = false;
    explicit Impl(int seconds, int quantizer, std::string codec, RifeFactory f,
                  std::shared_ptr<std::atomic<bool>> c, int timeout_seconds)
        : segment_seconds(seconds), qp(quantizer), encoder_name(std::move(codec)),
          factory(std::move(f)), cancelled(std::move(c)), source_timeout(timeout_seconds) {}
    ~Impl() {
        avcodec_free_context(&encoder);
        av_buffer_unref(&frames);
        av_buffer_unref(&device);
        sws_freeContext(sws);
        avcodec_free_context(&decoder);
        avformat_close_input(&input);
        avformat_close_input(&audio_input);
    }
    void check() {
        if (cancelled->load())
            throw Error(410, "Interpolation session was closed");
        if (deadline != Clock::time_point{} && Clock::now() >= deadline)
            throw Error(504, std::string(phase) + " exceeded the total time limit of " +
                                 std::to_string(source_timeout.count()) + " seconds");
    }
    bool interrupted() const {
        const auto now = Clock::now();
        return cancelled->load() || (deadline != Clock::time_point{} && now >= deadline) ||
               (attempt_deadline != Clock::time_point{} && now >= attempt_deadline);
    }
    void sourcecheck(int value, const char *operation) {
        check();
        if (value < 0 && attempt_deadline != Clock::time_point{} &&
            Clock::now() >= attempt_deadline)
            throw SourceError(504, std::string(operation) + " timed out while reading the source");
        modeld::sourcecheck(value, operation);
    }
    static int interrupt(void *opaque) {
        return static_cast<Impl *>(opaque)->interrupted() ? 1 : 0;
    }
    void capture_packet(const AVPacket *packet, bool separate) {
        for (auto &track : copy_tracks) {
            if (track->separate != separate || track->source_index != packet->stream_index ||
                track->parameters->codec_type == AVMEDIA_TYPE_ATTACHMENT)
                continue;
            if (packet->flags & AV_PKT_FLAG_CORRUPT)
                throw SourceError(502, "Incomplete source audio/subtitle packet");
            const int64_t timestamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
            if (timestamp == AV_NOPTS_VALUE)
                continue;
            const double start = timestamp * av_q2d(track->time_base) - track->origin;
            track->latest_end =
                std::max(track->latest_end,
                         start + std::max<int64_t>(packet->duration, 0) * av_q2d(track->time_base));
            if (track->parameters->codec_type == AVMEDIA_TYPE_SUBTITLE &&
                std::any_of(track->packets.begin(), track->packets.end(), [&](const auto &p) {
                    return p->pts == packet->pts && p->duration == packet->duration &&
                           p->size == packet->size && !std::memcmp(p->data, packet->data, p->size);
                }))
                continue;
            if (track->packets.size() >= 8192)
                throw SourceError(502, "Source media interleaving exceeds the packet limit");
            track->packets.push_back(copy_packet(packet));
        }
    }
    void configure_passthrough() {
        avformat_close_input(&audio_input);
        audio_eof = false;
        if (selection.audio && audio_source) {
            for (size_t attempt = 0;; ++attempt) {
                check();
                attempt_deadline = std::min(deadline, Clock::now() + std::chrono::seconds(5));
                avformat_close_input(&audio_input);
                audio_input = avformat_alloc_context();
                if (!audio_input)
                    throw std::bad_alloc();
                audio_input->interrupt_callback = {Impl::interrupt, this};
                AVDictionary *settings = nullptr;
                av_dict_set(&settings, "rw_timeout", "5000000", 0);
                av_dict_set(&settings, "reconnect", "1", 0);
                av_dict_set(&settings, "reconnect_on_network_error", "1", 0);
                av_dict_set(&settings, "reconnect_on_http_error", "408,429,5xx", 0);
                av_dict_set(&settings, "reconnect_max_retries", "1", 0);
                av_dict_set(&settings, "protocol_whitelist", "http,https,tcp,tls,crypto", 0);
                const auto headers = audio_source->http_headers();
                if (!headers.empty())
                    av_dict_set(&settings, "headers", headers.c_str(), 0);
                const int opened = avformat_open_input(
                    &audio_input, audio_source->urls.at(audio_index).c_str(), nullptr, &settings);
                av_dict_free(&settings);
                try {
                    sourcecheck(opened, "Open source audio");
                    sourcecheck(avformat_find_stream_info(audio_input, nullptr),
                                "Probe source audio");
                    if (av_find_best_stream(audio_input, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) <
                        0)
                        throw Error(422, "Separate input contains no audio track");
                    break;
                } catch (const SourceError &) {
                    check();
                    if (attempt + 1 >= audio_source->urls.size())
                        throw;
                    audio_index = (audio_index + 1) % audio_source->urls.size();
                }
            }
            attempt_deadline = std::min(deadline, Clock::now() + std::chrono::seconds(10));
        }
        if (!copy_tracks.empty()) {
            for (const auto &track : copy_tracks) {
                auto *format = track->separate ? audio_input : input;
                if (!format || track->source_index >= static_cast<int>(format->nb_streams))
                    throw Error(422, "Backup source track layout changed");
                const auto *parameters = format->streams[track->source_index]->codecpar;
                if (parameters->codec_id != track->parameters->codec_id ||
                    parameters->sample_rate != track->parameters->sample_rate ||
                    parameters->ch_layout.nb_channels != track->parameters->ch_layout.nb_channels ||
                    parameters->extradata_size != track->parameters->extradata_size ||
                    (parameters->extradata_size > 0 &&
                     std::memcmp(parameters->extradata, track->parameters->extradata,
                                 parameters->extradata_size)))
                    throw Error(422, "Backup source media tracks changed");
            }
            return;
        }
        auto add = [&](AVFormatContext *format, bool separate) {
            if (!format)
                return;
            for (unsigned n = 0; n < format->nb_streams; ++n) {
                const auto *stream = format->streams[n];
                const auto type = stream->codecpar->codec_type;
                const bool audio =
                    type == AVMEDIA_TYPE_AUDIO && selection.audio && (separate || !audio_source);
                const bool subtitle =
                    type == AVMEDIA_TYPE_SUBTITLE && selection.subtitles && !separate;
                const bool attachment =
                    type == AVMEDIA_TYPE_ATTACHMENT && selection.subtitles && !separate;
                if (!audio && !subtitle && !attachment)
                    continue;
                auto track = std::make_unique<CopyTrack>();
                if (!track->parameters)
                    throw std::bad_alloc();
                avcheck(avcodec_parameters_copy(track->parameters, stream->codecpar),
                        "Copy source track parameters");
                av_dict_copy(&track->metadata, stream->metadata, 0);
                track->source_index = n;
                track->time_base = stream->time_base;
                track->disposition = stream->disposition;
                track->separate = separate;
                track->origin = separate && format->start_time != AV_NOPTS_VALUE
                                    ? format->start_time / static_cast<double>(AV_TIME_BASE)
                                    : origin;
                copy_tracks.push_back(std::move(track));
            }
        };
        add(input, false);
        add(audio_input, true);
    }
    void collect_passthrough(double end) {
        for (const bool separate : {false, true}) {
            auto *format = separate ? audio_input : input;
            if (!format)
                continue;
            for (;;) {
                const bool ready =
                    std::all_of(copy_tracks.begin(), copy_tracks.end(), [&](const auto &t) {
                        return t->separate != separate ||
                               t->parameters->codec_type != AVMEDIA_TYPE_AUDIO ||
                               t->latest_end >= end - 0.0001;
                    });
                if (ready || (separate ? audio_eof : eof))
                    break;
                check();
                Packet packet;
                const int result = av_read_frame(format, packet.p);
                if (result == AVERROR_EOF) {
                    if (separate)
                        audio_eof = true;
                    else
                        eof = true;
                    break;
                }
                sourcecheck(result, "Read source audio packets");
                if (queued_video.size() >= 4096)
                    throw SourceError(502, "Source audio is too far behind the video");
                if (!separate && packet.p->stream_index == stream)
                    queued_video.push_back(copy_packet(packet.p));
                else
                    capture_packet(packet.p, separate);
            }
        }
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
            if (eof && queued_video.empty()) {
                if (flushed)
                    return nullptr;
                sourcecheck(avcodec_send_packet(decoder, nullptr), "Flush decoder");
                flushed = true;
                continue;
            }
            for (;;) {
                if (!queued_video.empty()) {
                    av_packet_ref(packet.p, queued_video.front().get());
                    queued_video.pop_front();
                    r = 0;
                } else {
                    r = av_read_frame(input, packet.p);
                    if (r >= 0 && packet.p->stream_index != stream)
                        capture_packet(packet.p, false);
                }
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
                : (std::max(info.width, info.height) >= 1280 ? SWS_CS_ITU709 : SWS_CS_ITU601);
        const int *coefficients = sws_getCoefficients(colors);
        sws_setColorspaceDetails(sws, coefficients, frame->color_range == AVCOL_RANGE_JPEG,
                                 coefficients, 1, 0, 1 << 16, 1 << 16);
        if (!rgb_frame) {
            rgb_frame = new_frame();
            rgb_frame->format = AV_PIX_FMT_RGB24;
            rgb_frame->width = info.width;
            rgb_frame->height = info.height;
            avcheck(av_frame_get_buffer(rgb_frame.get(), 64), "Allocate aligned RGB frame");
        }
        // swscale's SIMD converters can write beyond a packed row (24 bytes
        // past the last row at width 1080 on x86). Let FFmpeg allocate aligned
        // rows and padding, then copy the visible pixels to RIFE's packed input.
        avcheck(sws_scale(sws, frame->data, frame->linesize, 0, info.height, rgb_frame->data,
                          rgb_frame->linesize),
                "Convert source to RGB");
        for (int y = 0; y < info.height; ++y)
            std::memcpy(out->rgb.data() + static_cast<size_t>(y) * info.width * 3,
                        rgb_frame->data[0] + static_cast<size_t>(y) * rgb_frame->linesize[0],
                        static_cast<size_t>(info.width) * 3);
        return out;
    }
    void seek(double time) {
        auto *video = input->streams[stream];
        auto target =
            static_cast<int64_t>((std::max(0.0, time) + origin) / av_q2d(video->time_base));
        sourcecheck(av_seek_frame(input, stream, target, AVSEEK_FLAG_BACKWARD), "Seek source");
        avcodec_flush_buffers(decoder);
        queued_video.clear();
        for (auto &track : copy_tracks) {
            if (track->parameters->codec_type == AVMEDIA_TYPE_AUDIO)
                track->packets.clear();
            track->latest_end = -1;
        }
        if (audio_input) {
            const auto target = static_cast<int64_t>(time * AV_TIME_BASE);
            sourcecheck(av_seek_frame(audio_input, -1, target, AVSEEK_FLAG_BACKWARD),
                        "Seek source audio");
            audio_eof = false;
        }
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
        queued_video.clear();
        for (auto &track : copy_tracks) {
            if (track->parameters->codec_type == AVMEDIA_TYPE_AUDIO)
                track->packets.clear();
            track->latest_end = -1;
        }
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
        if (!supported_dimensions(p->width, p->height))
            throw Error(422,
                        "Unsupported native dimensions " + std::to_string(p->width) + "x" +
                            std::to_string(p->height) +
                            "; even sizes from 32x32 up to 1920x1080 or 1080x1920 are supported");
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
        configure_passthrough();
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
        encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
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
        if (fragmented_mp4 && codec->id == AV_CODEC_ID_H264) {
            av_dict_set(&options, "profile", "high", 0);
            av_dict_set(&options, "level", "4.2", 0);
        }
        if (fragmented_mp4 && codec->id == AV_CODEC_ID_AV1) {
            // NV12 selects Main profile, 8-bit. Keep the level stable when seeking.
            av_dict_set(&options, "level", "4.1", 0);
            av_dict_set_int(&options, "tier", 0, 0);
        }
        int r = avcodec_open2(encoder, codec, &options);
        av_dict_free(&options);
        avcheck(r, "Open NVENC encoder");
    }
    void mux_header(Mux &mux) {
        avcheck(avformat_alloc_output_context2(&mux.format, nullptr,
                                               fragmented_mp4 ? "mp4" : "matroska", nullptr),
                "Create segment muxer");
        auto *out = avformat_new_stream(mux.format, nullptr);
        if (!out)
            throw std::bad_alloc();
        avcheck(avcodec_parameters_from_context(out->codecpar, encoder),
                "Configure segment stream");
        out->time_base = encoder->time_base;
        out->avg_frame_rate = encoder->framerate;
        {
            mux.format->flags |= AVFMT_FLAG_BITEXACT;
            av_dict_copy(&mux.format->metadata, input->metadata, 0);
            for (const auto &track : copy_tracks) {
                auto *stream = avformat_new_stream(mux.format, nullptr);
                if (!stream)
                    throw std::bad_alloc();
                avcheck(avcodec_parameters_copy(stream->codecpar, track->parameters),
                        "Configure copied output track");
                stream->codecpar->codec_tag = 0;
                stream->time_base = track->time_base;
                stream->disposition = track->disposition;
                av_dict_copy(&stream->metadata, track->metadata, 0);
            }
        }
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
        AVDictionary *settings = nullptr;
        if (fragmented_mp4) {
            av_dict_set(
                &settings, "movflags",
                "empty_moov+frag_custom+default_base_moof+skip_trailer+negative_cts_offsets", 0);
            av_dict_set(&settings, "use_editlist", "0", 0);
        } else {
            av_dict_set(&settings, "live", "1", 0);
            av_dict_set(&settings, "cluster_time_limit", "1000", 0);
        }
        const int result = avformat_write_header(mux.format, &settings);
        av_dict_free(&settings);
        avcheck(result, "Write segment header");
        avio_flush(mux.io);
        mux.first_dts.assign(mux.format->nb_streams, AV_NOPTS_VALUE);
        if (fragmented_mp4) {
            // Browsers select the recorded-stream decode path from the MP4
            // initialization data before MediaSource.duration is applied.
            write_fmp4_recording_duration(mux.bytes, info.duration);
        } else {
            // An empty, finite Cluster lets mpv probe the init fragment without
            // reporting a truncated Matroska file. Subsequent clusters follow
            // normally, with their original absolute timestamps.
            constexpr uint8_t empty_cluster[] = {0x1f, 0x43, 0xb6, 0x75, 0x83, 0xe7, 0x81, 0x00};
            mux.bytes.insert(mux.bytes.end(), std::begin(empty_cluster), std::end(empty_cluster));
        }
    }
    void mux_passthrough(Mux &mux, double begin, double end) {
        collect_passthrough(end);
        for (size_t n = 0; n < copy_tracks.size(); ++n) {
            auto &track = *copy_tracks[n];
            auto *output = mux.format->streams[n + 1];
            for (const auto &source : track.packets) {
                const int64_t timestamp = source->pts != AV_NOPTS_VALUE ? source->pts : source->dts;
                const double start = timestamp * av_q2d(track.time_base) - track.origin;
                const double finish = start + source->duration * av_q2d(track.time_base);
                if (start >= end || (track.parameters->codec_type == AVMEDIA_TYPE_AUDIO
                                         ? (begin > 0 && start < begin - 0.0001)
                                         : (source->duration > 0 && finish <= begin)))
                    continue;
                Packet packet;
                avcheck(av_packet_ref(packet.p, source.get()), "Reference copied source packet");
                const auto offset =
                    static_cast<int64_t>(std::llround(track.origin / av_q2d(track.time_base)));
                if (packet.p->pts != AV_NOPTS_VALUE)
                    packet.p->pts -= offset;
                if (packet.p->dts != AV_NOPTS_VALUE)
                    packet.p->dts -= offset;
                av_packet_rescale_ts(packet.p, track.time_base, output->time_base);
                packet.p->stream_index = output->index;
                mux.packet(packet.p);
            }
            std::erase_if(track.packets, [&](const auto &p) {
                const auto ts = p->pts != AV_NOPTS_VALUE ? p->pts : p->dts;
                const double start = ts * av_q2d(track.time_base) - track.origin;
                return track.parameters->codec_type == AVMEDIA_TYPE_AUDIO
                           ? start < end
                           : p->duration > 0 &&
                                 start + p->duration * av_q2d(track.time_base) <= begin;
            });
        }
    }
};
MediaStream::MediaStream(Input video, std::optional<Input> audio, TrackSelection tracks,
                         RenderSettings settings, RifeFactory factory,
                         std::shared_ptr<std::atomic<bool>> cancelled)
    : impl(std::make_unique<Impl>(settings.segment_seconds, settings.qp, settings.encoder,
                                  std::move(factory), std::move(cancelled),
                                  settings.source_timeout_seconds)) {
    auto &v = *impl;
    v.fragmented_mp4 = settings.fragmented_mp4;
    v.selection = tracks;
    v.audio_source = std::move(audio);
    if (settings.source_timeout_seconds < 1 || settings.source_timeout_seconds > 120)
        throw Error(422, "Source timeout must be between 1 and 120 seconds");
    v.deadline = Clock::now() + v.source_timeout;
    v.sources = std::move(video.urls);
    v.source_headers = video.http_headers();
    for (;;) {
        try {
            // One stalled primary must leave time to try a healthy backup.
            v.attempt_deadline = std::min(v.deadline, Clock::now() + std::chrono::seconds(10));
            v.open_source(v.source_codec == AV_CODEC_ID_NONE);
            break;
        } catch (const SourceError &e) {
            std::cerr << "[modeld] Source probe failed: source " << v.source_index + 1 << '/'
                      << v.sources.size() << ": " << e.what() << '\n';
            v.check();
            if (++v.source_index >= v.sources.size())
                throw;
            std::cerr << "[modeld] Initial source failed; trying backup " << v.source_index + 1
                      << '\n';
        }
    }
    if (v.fragmented_mp4) {
        if (tracks.subtitles ||
            (settings.encoder != "h264_nvenc" && settings.encoder != "av1_nvenc"))
            throw Error(422, "Browser output requires H.264 or AV1 without embedded subtitles");
        int audio_tracks = 0;
        for (const auto &track : v.copy_tracks) {
            if (track->parameters->codec_type != AVMEDIA_TYPE_AUDIO ||
                track->parameters->codec_id != AV_CODEC_ID_AAC || ++audio_tracks > 1)
                throw Error(422, "Browser output supports one AAC audio track");
            if (track->parameters->profile != AV_PROFILE_AAC_LOW)
                throw Error(422, "Browser output requires AAC-LC audio");
        }
    }
    {
        v.prepare_encoder();
        Mux mux;
        v.mux_header(mux);
        v.init_header = std::move(mux.bytes);
    }
    v.deadline = v.attempt_deadline = {};
}
MediaStream::~MediaStream() = default;
Json MediaStream::description() const {
    const auto &v = *impl;
    Json tracks = Json::array({{{"kind", "video"},
                                {"id", 1},
                                {"source", "video"},
                                {"source_index", v.stream},
                                {"codec", avcodec_get_name(v.encoder->codec_id)},
                                {"language", ""},
                                {"title", ""}}});
    int audio = 0, subtitles = 0, attachments = 0;
    for (const auto &track : v.copy_tracks) {
        const auto type = track->parameters->codec_type;
        const auto *language = av_dict_get(track->metadata, "language", nullptr, 0);
        const auto *title = av_dict_get(track->metadata, "title", nullptr, 0);
        const auto kind = type == AVMEDIA_TYPE_AUDIO      ? "audio"
                          : type == AVMEDIA_TYPE_SUBTITLE ? "subtitle"
                                                          : "attachment";
        const auto id = type == AVMEDIA_TYPE_AUDIO      ? ++audio
                        : type == AVMEDIA_TYPE_SUBTITLE ? ++subtitles
                                                        : ++attachments;
        tracks.push_back({{"kind", kind},
                          {"id", id},
                          {"source", track->separate ? "audio" : "video"},
                          {"source_index", track->source_index},
                          {"codec", avcodec_get_name(track->parameters->codec_id)},
                          {"language", language ? language->value : ""},
                          {"title", title ? title->value : ""}});
    }
    const auto frames = static_cast<int64_t>(std::ceil(v.info.duration * 60));
    const int64_t per_segment = v.segment_seconds * 60;
    return {{"width", v.info.width},
            {"height", v.info.height},
            {"duration_seconds", v.info.duration},
            {"source_fps", v.info.source_fps},
            {"output_fps", 60},
            {"segment_seconds", v.segment_seconds},
            {"segment_count", (frames + per_segment - 1) / per_segment},
            {"tracks", tracks}};
}
std::vector<uint8_t> MediaStream::initialization() const {
    return impl->init_header;
}
std::string MediaStream::content_type() const {
    if (!impl->fragmented_mp4)
        return "video/x-matroska";
    const char *video_codec = impl->encoder_name == "av1_nvenc" ? "av01.0.09M.08" : "avc1.64002A";
    return std::string("video/mp4; codecs=\"") + video_codec +
           (impl->copy_tracks.empty() ? "" : ",mp4a.40.2") + "\"";
}
Segment MediaStream::render(int64_t sequence) {
    auto started = Clock::now();
    auto &v = *impl;
    v.check();
    // Profile preparation is startup work, not a stalled source read. It is
    // still included in render_seconds, and owns its session slot until done.
    if (!v.rife)
        v.rife = v.factory(v.info.width, v.info.height);
    v.check();
    v.phase = "Source segment";
    v.deadline = Clock::now() + v.source_timeout;
    struct DeadlineReset {
        Impl &v;
        ~DeadlineReset() {
            v.deadline = v.attempt_deadline = {};
        }
    } reset{v};
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
        Mux mux;
        v.mux_header(mux);
        auto *out = mux.format->streams[0];
        {
            if (mux.bytes != v.init_header)
                throw Error(502, "Output track header changed during playback");
            mux.bytes.clear();
        }
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
                mux.packet(packet.p);
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
        v.mux_passthrough(mux, begin / 60.0, end / 60.0);
        if (packets != end - begin)
            throw Error(500, "NVENC output was delayed; zero-latency configuration is required");
        avcheck(av_write_trailer(mux.format), "Finish segment");
        avio_flush(mux.io);
        if (v.fragmented_mp4)
            mux.absolute_decode_times();
        v.next_segment = sequence + 1;
        return {std::move(mux.bytes), static_cast<int>(end - begin),
                std::chrono::duration<double>(Clock::now() - started).count()};
    };
    for (int attempt = 0;; ++attempt) {
        try {
            return render_once();
        } catch (const SourceError &e) {
            v.source_ready = false;
            v.next_segment = -2;
            v.check();
            std::cerr << "[modeld] Source recovery: segment " << sequence << ", attempt "
                      << attempt + 1 << ": " << e.what() << '\n';
            v.source_index = (v.source_index + 1) % v.sources.size();
            if (v.audio_source)
                v.audio_index = (v.audio_index + 1) % v.audio_source->urls.size();
            if (attempt >= 1)
                throw;
        } catch (...) {
            // Any aborted segment may have advanced the decoder. A later
            // request must reopen and seek, including a retry of segment zero.
            v.source_ready = false;
            v.next_segment = -2;
            throw;
        }
    }
}
} // namespace modeld
