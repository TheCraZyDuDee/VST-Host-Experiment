#include "pipewire_engine.hpp"

#include "stereo_ring_buffer.hpp"

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <utility>

namespace vst_host::audio {
namespace {

constexpr std::uint32_t sample_rate = 48000;
constexpr std::uint32_t channel_count = 2;

struct EngineState {
    pw_main_loop* loop{};
    pw_stream* capture_stream{};
    pw_filter* source_filter{};
    void* left_port{};
    void* right_port{};
    StereoRingBuffer ring;
    float gain{1.0F};
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
        state->ring.push_interleaved(static_cast<const float*>(data.data), frames);
    }

    pw_stream_queue_buffer(state->capture_stream, buffer);
}

void on_capture_state_changed(void*, pw_stream_state, pw_stream_state state,
                              const char* error) {
    if (state == PW_STREAM_STATE_ERROR) {
        std::fprintf(stderr, "PipeWire capture stream error: %s\n",
                     error != nullptr ? error : "unknown error");
    }
}

void on_source_process(void* userdata, spa_io_position* position) {
    auto* state = static_cast<EngineState*>(userdata);
    const auto frames = static_cast<std::size_t>(position->clock.duration);

    auto* left = static_cast<float*>(pw_filter_get_dsp_buffer(
        state->left_port, static_cast<std::uint32_t>(frames)));
    auto* right = static_cast<float*>(pw_filter_get_dsp_buffer(
        state->right_port, static_cast<std::uint32_t>(frames)));
    if (left == nullptr || right == nullptr) {
        return;
    }

    state->ring.pop_planar(left, right, frames);
    if (!state->bypass && state->gain != 1.0F) {
        for (std::size_t i = 0; i < frames; ++i) {
            left[i] *= state->gain;
            right[i] *= state->gain;
        }
    }
}

void on_source_state_changed(void* userdata, pw_filter_state, pw_filter_state state,
                             const char* error) {
    auto* data = static_cast<EngineState*>(userdata);
    if (state == PW_FILTER_STATE_ERROR) {
        std::fprintf(stderr, "PipeWire virtual source error: %s\n",
                     error != nullptr ? error : "unknown error");
        data->setup_error = -EIO;
        if (data->loop != nullptr) {
            pw_main_loop_quit(data->loop);
        }
    }
}

const pw_stream_events capture_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_capture_state_changed,
    .process = on_capture_process,
};

const pw_filter_events source_events = {
    PW_VERSION_FILTER_EVENTS,
    .state_changed = on_source_state_changed,
    .process = on_source_process,
};

} // namespace

struct PipeWireEngine::Impl {
    explicit Impl(PipeWireOptions supplied_options)
        : options(std::move(supplied_options)) {
        state.gain = options.gain;
        state.bypass = options.bypass;
    }

    PipeWireOptions options;
    EngineState state;
    spa_hook capture_listener{};
    bool initialized{false};

    int create_source() {
        auto* properties = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Source",
            PW_KEY_MEDIA_ROLE, "Communication",
            PW_KEY_MEDIA_CLASS, "Audio/Source/Virtual",
            PW_KEY_NODE_NAME, options.output_name.c_str(),
            PW_KEY_NODE_DESCRIPTION, options.output_description.c_str(),
            PW_KEY_AUDIO_CHANNELS, "2",
            PW_KEY_NODE_VIRTUAL, "true",
            nullptr);
        if (properties == nullptr) {
            return -ENOMEM;
        }

        state.source_filter = pw_filter_new_simple(
            pw_main_loop_get_loop(state.loop),
            options.output_name.c_str(),
            properties,
            &source_events,
            &state);

        if (state.source_filter == nullptr) {
            std::fprintf(stderr, "Could not create PipeWire virtual source.\n");
            return -ENOMEM;
        }

        state.left_port = pw_filter_add_port(
            state.source_filter, PW_DIRECTION_OUTPUT,
            PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(void*),
            pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                              PW_KEY_PORT_NAME, "front-left",
                              PW_KEY_AUDIO_CHANNEL, "FL", nullptr),
            nullptr, 0);
        state.right_port = pw_filter_add_port(
            state.source_filter, PW_DIRECTION_OUTPUT,
            PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(void*),
            pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                              PW_KEY_PORT_NAME, "front-right",
                              PW_KEY_AUDIO_CHANNEL, "FR", nullptr),
            nullptr, 0);

        if (state.left_port == nullptr || state.right_port == nullptr) {
            std::fprintf(stderr, "Could not create virtual source audio ports.\n");
            return -ENOMEM;
        }

        if (pw_filter_connect(state.source_filter, PW_FILTER_FLAG_RT_PROCESS,
                              nullptr, 0) < 0) {
            std::fprintf(stderr, "Could not connect the virtual source to PipeWire.\n");
            return -EIO;
        }
        return 0;
    }

    int create_capture_stream() {
        auto* properties = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Communication",
            PW_KEY_NODE_DESCRIPTION, "VST Host Microphone Capture",
            nullptr);
        if (properties == nullptr) {
            return -ENOMEM;
        }
        if (!options.input_target.empty()) {
            pw_properties_set(properties, PW_KEY_TARGET_OBJECT,
                              options.input_target.c_str());
        }

        state.capture_stream = pw_stream_new(
            pw_filter_get_core(state.source_filter),
            "vst-host-capture",
            properties);
        if (state.capture_stream == nullptr) {
            std::fprintf(stderr, "Could not create PipeWire capture stream.\n");
            return -ENOMEM;
        }

        std::uint8_t pod_buffer[1024];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
        const spa_audio_info_raw format = SPA_AUDIO_INFO_RAW_INIT(
            .format = SPA_AUDIO_FORMAT_F32,
            .channels = channel_count,
            .rate = sample_rate);
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
    if (impl_->state.source_filter != nullptr) {
        pw_filter_destroy(impl_->state.source_filter);
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

    pw_loop_add_signal(pw_main_loop_get_loop(impl_->state.loop), SIGINT,
                       request_shutdown, &impl_->state);
    pw_loop_add_signal(pw_main_loop_get_loop(impl_->state.loop), SIGTERM,
                       request_shutdown, &impl_->state);

    auto result = impl_->create_source();
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
        std::printf("Press Ctrl+C to stop.\n");
        pw_main_loop_run(impl_->state.loop);
        result = impl_->state.setup_error;
    }

    return result;
}

} // namespace vst_host::audio
