#include "pipewire_engine.hpp"

#include "stereo_ring_buffer.hpp"

#include <pipewire/pipewire.h>

#include <algorithm>
#include <spa/param/audio/format-utils.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace vst_host::audio {
namespace {

constexpr std::uint32_t sample_rate = 48000;
constexpr std::uint32_t channel_count = 2;
constexpr std::size_t frame_bytes = sizeof(float) * channel_count;

struct EngineState {
    pw_main_loop* loop{};
    pw_context* context{};
    pw_core* core{};
    pw_stream* capture_stream{};
    pw_stream* source_stream{};
    StereoRingBuffer ring;
    float gain{1.0F};
    std::size_t latency_frames{256};
    bool bypass{false};
    int setup_error{0};
};

void request_shutdown(void* userdata, int) {
    auto* state = static_cast<EngineState*>(userdata);
    if (state->loop != nullptr) {
        pw_main_loop_quit(state->loop);
    }
}

void on_capture_process(void* userdata) {
    auto* state = static_cast<EngineState*>(userdata);
    auto* buffer = pw_stream_dequeue_buffer(state->capture_stream);
    if (buffer == nullptr) {
        return;
    }

    auto* spa_buffer = buffer->buffer;
    if (spa_buffer != nullptr && spa_buffer->n_datas > 0 &&
        spa_buffer->datas[0].data != nullptr &&
        spa_buffer->datas[0].chunk != nullptr) {
        const auto& data = spa_buffer->datas[0];
        const auto sample_count = static_cast<std::size_t>(data.chunk->size) /
                                  sizeof(float);
        const auto frames = sample_count / channel_count;
        const auto* samples = reinterpret_cast<const float*>(
            static_cast<const std::uint8_t*>(data.data) + data.chunk->offset);
        state->ring.push_interleaved(samples, frames);
    }

    pw_stream_queue_buffer(state->capture_stream, buffer);
}

void on_capture_state_changed(void* userdata, pw_stream_state, pw_stream_state state,
                              const char* error) {
    if (state == PW_STREAM_STATE_ERROR) {
        auto* data = static_cast<EngineState*>(userdata);
        std::fprintf(stderr, "PipeWire capture stream error: %s\n",
                     error != nullptr ? error : "unknown error");
        data->setup_error = -EIO;
        if (data->loop != nullptr) {
            pw_main_loop_quit(data->loop);
        }
    }
}

void on_source_process(void* userdata) {
    auto* state = static_cast<EngineState*>(userdata);
    auto* buffer = pw_stream_dequeue_buffer(state->source_stream);
    if (buffer == nullptr) {
        return;
    }

    auto* spa_buffer = buffer->buffer;
    if (spa_buffer != nullptr && spa_buffer->n_datas > 0 &&
        spa_buffer->datas[0].data != nullptr &&
        spa_buffer->datas[0].chunk != nullptr) {
        auto& data = spa_buffer->datas[0];
        auto frames = static_cast<std::size_t>(data.maxsize) / frame_bytes;
        if (buffer->requested > 0) {
            frames = std::min(frames, static_cast<std::size_t>(buffer->requested));
        }
        auto* samples = reinterpret_cast<float*>(data.data);

        state->ring.pop_interleaved(samples, frames, state->latency_frames * 2);
        if (!state->bypass && state->gain != 1.0F) {
            for (std::size_t i = 0; i < frames * channel_count; ++i) {
                samples[i] *= state->gain;
            }
        }

        data.chunk->offset = 0;
        data.chunk->stride = static_cast<int32_t>(frame_bytes);
        data.chunk->size = static_cast<std::uint32_t>(frames * frame_bytes);
    }

    pw_stream_queue_buffer(state->source_stream, buffer);
}

void on_source_state_changed(void* userdata, pw_stream_state, pw_stream_state state,
                             const char* error) {
    auto* data = static_cast<EngineState*>(userdata);
    if (state == PW_STREAM_STATE_ERROR) {
        std::fprintf(stderr, "PipeWire virtual microphone error: %s\n",
                     error != nullptr ? error : "unknown error");
        data->setup_error = -EIO;
        if (data->loop != nullptr) {
            pw_main_loop_quit(data->loop);
        }
    }
}

const pw_stream_events capture_events = [] {
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = on_capture_state_changed;
    events.process = on_capture_process;
    return events;
}();

const pw_stream_events source_events = [] {
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = on_source_state_changed;
    events.process = on_source_process;
    return events;
}();

} // namespace

struct PipeWireEngine::Impl {
    explicit Impl(PipeWireOptions supplied_options)
        : options(std::move(supplied_options)) {
        state.gain = options.gain;
        state.latency_frames = options.latency_frames;
        state.bypass = options.bypass;
    }

    PipeWireOptions options;
    EngineState state;
    spa_hook capture_listener{};
    spa_hook source_listener{};
    bool initialized{false};

    int create_source_stream() {
        const auto latency = std::to_string(options.latency_frames) + "/48000";
        auto* properties = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Source",
            PW_KEY_MEDIA_ROLE, "Communication",
            // A pw_stream producing audio is the native PipeWire source API.
            // Unlike a pw_filter, it is exported as a capture source to clients.
            PW_KEY_MEDIA_CLASS, "Audio/Source",
            PW_KEY_NODE_NAME, options.output_name.c_str(),
            PW_KEY_NODE_DESCRIPTION, options.output_description.c_str(),
            PW_KEY_NODE_LATENCY, latency.c_str(),
            PW_KEY_AUDIO_CHANNELS, "2",
            PW_KEY_NODE_VIRTUAL, "true",
            nullptr);
        if (properties == nullptr) {
            return -ENOMEM;
        }

        state.source_stream = pw_stream_new(
            state.core, options.output_name.c_str(), properties);
        if (state.source_stream == nullptr) {
            std::fprintf(stderr, "Could not create PipeWire virtual microphone stream.\n");
            return -ENOMEM;
        }

        pw_stream_add_listener(state.source_stream, &source_listener,
                               &source_events, &state);

        std::uint8_t pod_buffer[1024];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
        spa_audio_info_raw format{};
        format.format = SPA_AUDIO_FORMAT_F32;
        format.channels = channel_count;
        format.rate = sample_rate;
        format.position[0] = SPA_AUDIO_CHANNEL_FL;
        format.position[1] = SPA_AUDIO_CHANNEL_FR;
        const spa_pod* params[1];
        params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format);

        const auto connect_result = pw_stream_connect(
            state.source_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS |
                                         PW_STREAM_FLAG_RT_PROCESS),
            params, 1);
        if (connect_result < 0) {
            std::fprintf(stderr, "Could not publish virtual microphone: %s\n",
                         std::strerror(-connect_result));
            return connect_result;
        }
        return 0;
    }

    int create_capture_stream() {
        const auto latency = std::to_string(options.latency_frames) + "/48000";
        auto* properties = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Communication",
            PW_KEY_NODE_DESCRIPTION, "VST Host Microphone Capture",
            PW_KEY_NODE_LATENCY, latency.c_str(),
            nullptr);
        if (properties == nullptr) {
            return -ENOMEM;
        }
        if (!options.input_target.empty()) {
            pw_properties_set(properties, PW_KEY_TARGET_OBJECT,
                              options.input_target.c_str());
        }

        state.capture_stream = pw_stream_new(
            state.core, "vst-host-capture", properties);
        if (state.capture_stream == nullptr) {
            std::fprintf(stderr, "Could not create PipeWire capture stream.\n");
            return -ENOMEM;
        }

        std::uint8_t pod_buffer[1024];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
        spa_audio_info_raw format{};
        format.format = SPA_AUDIO_FORMAT_F32;
        format.channels = channel_count;
        format.rate = sample_rate;
        format.position[0] = SPA_AUDIO_CHANNEL_FL;
        format.position[1] = SPA_AUDIO_CHANNEL_FR;
        const spa_pod* params[1];
        params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format);

        pw_stream_add_listener(state.capture_stream, &capture_listener,
                               &capture_events, &state);

        const auto connect_result = pw_stream_connect(
            state.capture_stream, PW_DIRECTION_INPUT, PW_ID_ANY,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                         PW_STREAM_FLAG_MAP_BUFFERS |
                                         PW_STREAM_FLAG_RT_PROCESS),
            params, 1);
        if (connect_result < 0) {
            std::fprintf(stderr, "Could not connect to microphone: %s\n",
                         std::strerror(-connect_result));
            return connect_result;
        }
        return 0;
    }
};

PipeWireEngine::PipeWireEngine(PipeWireOptions options)
    : impl_(new Impl(std::move(options))) {}

PipeWireEngine::~PipeWireEngine() {
    if (impl_ == nullptr) {
        return;
    }
    if (impl_->state.capture_stream != nullptr) {
        pw_stream_destroy(impl_->state.capture_stream);
    }
    if (impl_->state.source_stream != nullptr) {
        pw_stream_destroy(impl_->state.source_stream);
    }
    if (impl_->state.core != nullptr) {
        pw_core_disconnect(impl_->state.core);
    }
    if (impl_->state.context != nullptr) {
        pw_context_destroy(impl_->state.context);
    }
    if (impl_->state.loop != nullptr) {
        pw_main_loop_destroy(impl_->state.loop);
    }
    if (impl_->initialized) {
        pw_deinit();
    }
    delete impl_;
}

int PipeWireEngine::run() {
    int argc = 0;
    char* arguments[] = {nullptr};
    char** argv = arguments;
    pw_init(&argc, &argv);
    impl_->initialized = true;

    impl_->state.loop = pw_main_loop_new(nullptr);
    if (impl_->state.loop == nullptr) {
        std::fprintf(stderr, "Could not create PipeWire main loop.\n");
        return -ENOMEM;
    }

    impl_->state.context = pw_context_new(
        pw_main_loop_get_loop(impl_->state.loop), nullptr, 0);
    if (impl_->state.context == nullptr) {
        std::fprintf(stderr, "Could not create PipeWire context.\n");
        return -ENOMEM;
    }
    impl_->state.core = pw_context_connect(impl_->state.context, nullptr, 0);
    if (impl_->state.core == nullptr) {
        std::fprintf(stderr, "Could not connect to PipeWire.\n");
        return -EIO;
    }

    pw_loop_add_signal(pw_main_loop_get_loop(impl_->state.loop), SIGINT,
                       request_shutdown, &impl_->state);
    pw_loop_add_signal(pw_main_loop_get_loop(impl_->state.loop), SIGTERM,
                       request_shutdown, &impl_->state);

    auto result = impl_->create_source_stream();
    if (result == 0) {
        result = impl_->create_capture_stream();
    }
    if (result == 0) {
        std::printf("Virtual microphone: %s\n", impl_->options.output_name.c_str());
        std::printf("Input: %s\n",
                    impl_->options.input_target.empty() ? "default microphone"
                                                        : impl_->options.input_target.c_str());
        std::printf("Gain: %.3f%s\n", impl_->options.gain,
                    impl_->options.bypass ? " (bypassed)" : "");
        std::printf("Requested latency: %zu frames (%.2f ms at 48 kHz)\n",
                    impl_->options.latency_frames,
                    1000.0 * static_cast<double>(impl_->options.latency_frames) / sample_rate);
        std::printf("Press Ctrl+C to stop.\n");
        pw_main_loop_run(impl_->state.loop);
        result = impl_->state.setup_error;
        const auto dropped = impl_->state.ring.dropped_frames();
        const auto underruns = impl_->state.ring.underrun_frames();
        std::printf("Audio diagnostics: dropped=%llu frames (%.2f ms), underrun=%llu frames (%.2f ms)\\n",
                    static_cast<unsigned long long>(dropped),
                    1000.0 * static_cast<double>(dropped) / sample_rate,
                    static_cast<unsigned long long>(underruns),
                    1000.0 * static_cast<double>(underruns) / sample_rate);
    }

    return result;
}

} // namespace vst_host::audio
