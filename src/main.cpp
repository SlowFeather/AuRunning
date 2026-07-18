#include <Windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <propkeydef.h>
#include <devpkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <wrl/client.h>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <nlohmann/json.hpp>

#include <common_audio/vad/include/webrtc_vad.h>
#include <common_audio/resampler/include/push_resampler.h>
#include <modules/audio_processing/include/audio_processing.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;
using json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

struct Config {
    std::string host = "127.0.0.1";
    int port = 8810;
    std::string ws_path = "/v1/audio/ws";
    int device_sample_rate = 48000;
    int capture_sample_rate = 16000;
    int frame_ms = 10;
    int render_queue_ms = 500;
    int capture_queue_ms = 1000;
    int capture_stall_timeout_ms = 3000;
    int aec_tail_ms = 200;
    int barge_in_min_speech_ms = 120;
    int barge_in_hangover_ms = 300;
    int vad_aggressiveness = 2;
    std::optional<std::string> input_device;
    std::optional<std::string> output_device;
};

Config load_config(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open config: " + path);
    json data;
    input >> data;
    Config cfg;
    cfg.host = data.value("host", cfg.host);
    cfg.port = data.value("port", cfg.port);
    cfg.ws_path = data.value("ws_path", cfg.ws_path);
    cfg.device_sample_rate = data.value("device_sample_rate", cfg.device_sample_rate);
    cfg.capture_sample_rate = data.value("capture_sample_rate", cfg.capture_sample_rate);
    cfg.frame_ms = data.value("frame_ms", cfg.frame_ms);
    cfg.render_queue_ms = data.value("render_queue_ms", cfg.render_queue_ms);
    cfg.capture_queue_ms = data.value("capture_queue_ms", cfg.capture_queue_ms);
    cfg.capture_stall_timeout_ms = data.value("capture_stall_timeout_ms", cfg.capture_stall_timeout_ms);
    cfg.aec_tail_ms = data.value("aec_tail_ms", cfg.aec_tail_ms);
    cfg.barge_in_min_speech_ms = data.value("barge_in_min_speech_ms", cfg.barge_in_min_speech_ms);
    cfg.barge_in_hangover_ms = data.value("barge_in_hangover_ms", cfg.barge_in_hangover_ms);
    cfg.vad_aggressiveness = data.value("vad_aggressiveness", cfg.vad_aggressiveness);
    if (data.contains("input_device") && !data["input_device"].is_null()) cfg.input_device = data["input_device"].get<std::string>();
    if (data.contains("output_device") && !data["output_device"].is_null()) cfg.output_device = data["output_device"].get<std::string>();
    if (cfg.device_sample_rate != 48000 || cfg.capture_sample_rate != 16000 || cfg.frame_ms != 10) {
        throw std::runtime_error(
            "AudioRuntime currently requires device_sample_rate=48000, capture_sample_rate=16000, and frame_ms=10"
        );
    }
    if (cfg.port < 1 || cfg.port > 65535 || cfg.render_queue_ms < 10 || cfg.capture_queue_ms < 10 ||
        cfg.capture_stall_timeout_ms < 1000 || cfg.aec_tail_ms < 10 ||
        cfg.barge_in_min_speech_ms <= 0 || cfg.barge_in_hangover_ms <= 0) {
        throw std::runtime_error("invalid audio runtime configuration");
    }
    if (cfg.vad_aggressiveness < 0 || cfg.vad_aggressiveness > 3) {
        throw std::runtime_error("vad_aggressiveness must be between 0 and 3");
    }
    return cfg;
}

void check_hr(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        char buffer[128];
        sprintf_s(buffer, "%s failed HRESULT=0x%08lx", operation, static_cast<unsigned long>(hr));
        throw std::runtime_error(buffer);
    }
}

std::wstring utf8_to_wide(const std::string& value) {
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    std::wstring result(static_cast<size_t>(std::max(0, count)), L'\0');
    if (count > 1) MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), count);
    if (!result.empty()) result.pop_back();
    return result;
}

std::string wide_to_utf8(const wchar_t* value) {
    if (!value) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(std::max(0, count)), '\0');
    if (count > 1) WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), count, nullptr, nullptr);
    if (!result.empty()) result.pop_back();
    return result;
}

struct DeviceSelection {
    ComPtr<IMMDevice> device;
    std::string name;
    std::wstring id;
};

std::wstring device_id(IMMDevice* device) {
    LPWSTR raw = nullptr;
    check_hr(device->GetId(&raw), "IMMDevice::GetId");
    std::wstring result = raw ? raw : L"";
    CoTaskMemFree(raw);
    return result;
}

DeviceSelection select_device(IMMDeviceEnumerator* enumerator, EDataFlow flow, const std::optional<std::string>& requested) {
    if (!requested || requested->empty()) {
        DeviceSelection result;
        check_hr(enumerator->GetDefaultAudioEndpoint(flow, eCommunications, &result.device), "GetDefaultAudioEndpoint");
        ComPtr<IPropertyStore> store;
        if (SUCCEEDED(result.device->OpenPropertyStore(STGM_READ, &store))) {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) result.name = wide_to_utf8(value.pwszVal);
            PropVariantClear(&value);
        }
        result.id = device_id(result.device.Get());
        return result;
    }

    ComPtr<IMMDeviceCollection> devices;
    check_hr(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &devices), "EnumAudioEndpoints");
    UINT count = 0;
    check_hr(devices->GetCount(&count), "IMMDeviceCollection::GetCount");
    const std::wstring wanted = utf8_to_wide(*requested);
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        check_hr(devices->Item(index, &device), "IMMDeviceCollection::Item");
        ComPtr<IPropertyStore> store;
        if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) continue;
        PROPVARIANT value;
        PropVariantInit(&value);
        const HRESULT hr = store->GetValue(PKEY_Device_FriendlyName, &value);
        const std::wstring name = SUCCEEDED(hr) && value.vt == VT_LPWSTR ? value.pwszVal : L"";
        PropVariantClear(&value);
        if (_wcsicmp(name.c_str(), wanted.c_str()) == 0 || name.find(wanted) != std::wstring::npos) {
            return {device, wide_to_utf8(name.c_str()), device_id(device.Get())};
        }
    }
    throw std::runtime_error("configured audio endpoint not found: " + *requested);
}

std::vector<float> resample_pcm16_mono(const std::string& pcm, int source_rate, int channels, int destination_rate) {
    if (pcm.size() % (sizeof(int16_t) * static_cast<size_t>(channels)) != 0 || source_rate <= 0 || channels <= 0) {
        throw std::runtime_error("invalid PCM chunk");
    }
    const auto* samples = reinterpret_cast<const int16_t*>(pcm.data());
    const size_t source_frames = pcm.size() / (sizeof(int16_t) * static_cast<size_t>(channels));
    if (source_frames == 0) return {};
    std::vector<float> mono(source_frames);
    for (size_t frame = 0; frame < source_frames; ++frame) {
        float sum = 0.0f;
        for (int channel = 0; channel < channels; ++channel) sum += samples[frame * channels + channel] / 32768.0f;
        mono[frame] = sum / static_cast<float>(channels);
    }
    const size_t destination_frames = std::max<size_t>(1, static_cast<size_t>(std::llround(source_frames * static_cast<double>(destination_rate) / source_rate)));
    std::vector<float> output(destination_frames);
    const double step = static_cast<double>(source_rate) / destination_rate;
    for (size_t index = 0; index < destination_frames; ++index) {
        const double position = std::min(static_cast<double>(source_frames - 1), index * step);
        const size_t left = static_cast<size_t>(position);
        const size_t right = std::min(left + 1, source_frames - 1);
        const float fraction = static_cast<float>(position - left);
        output[index] = mono[left] + (mono[right] - mono[left]) * fraction;
    }
    return output;
}

class AudioEngine {
public:
    using BinaryCallback = std::function<void(std::string)>;
    using EventCallback = std::function<void(const json&)>;

    explicit AudioEngine(Config cfg) : cfg_(std::move(cfg)) {}
    ~AudioEngine() { stop(); }

    void set_callbacks(BinaryCallback binary, EventCallback event) {
        binary_callback_ = std::move(binary);
        event_callback_ = std::move(event);
    }

    bool start() {
        try {
            initialize();
            restart_requested_ = false;
            running_ = true;
            check_hr(render_client_->Start(), "render IAudioClient::Start");
            check_hr(capture_client_->Start(), "capture IAudioClient::Start");
            const ULONGLONG started = GetTickCount64();
            last_capture_tick_.store(started);
            last_render_progress_tick_.store(started);
            render_thread_ = std::thread(&AudioEngine::render_loop, this);
            capture_thread_ = std::thread(&AudioEngine::capture_loop, this);
            ready_ = true;
            return true;
        } catch (const std::exception& exc) {
            set_last_error(exc.what());
            ready_ = false;
            return false;
        }
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (render_event_) SetEvent(render_event_);
        if (capture_event_) SetEvent(capture_event_);
        render_cv_.notify_all();
        if (render_thread_.joinable()) render_thread_.join();
        if (capture_thread_.joinable()) capture_thread_.join();
        if (render_client_) render_client_->Stop();
        if (capture_client_) capture_client_->Stop();
        if (render_event_) CloseHandle(render_event_);
        if (capture_event_) CloseHandle(capture_event_);
        render_event_ = nullptr;
        capture_event_ = nullptr;
        if (vad_) WebRtcVad_Free(vad_);
        vad_ = nullptr;
        apm_.reset();
        ready_ = false;
    }

    json status() {
        std::string last_error;
        {
            std::lock_guard lock(error_mutex_);
            last_error = last_error_;
        }
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG last_capture = last_capture_tick_.load();
        const ULONGLONG last_render_progress = last_render_progress_tick_.load();
        json aec_metrics = {
            {"capture_frames", capture_frames_.load()},
            {"reverse_frames", reverse_frames_.load()},
            {"process_errors", process_errors_.load()},
            {"near_end_events", near_end_events_.load()},
            {"resampled_frames", resampled_frames_.load()},
            {"capture_device_errors", capture_device_errors_.load()},
            {"render_device_errors", render_device_errors_.load()},
            {"last_capture_age_ms", last_capture == 0 ? nullptr : json(now - last_capture)},
            {"last_render_progress_age_ms", last_render_progress == 0 ? nullptr : json(now - last_render_progress)}
        };
        json effective_config = {
            {"device_sample_rate", cfg_.device_sample_rate},
            {"capture_sample_rate", cfg_.capture_sample_rate},
            {"frame_ms", cfg_.frame_ms},
            {"render_queue_ms", cfg_.render_queue_ms},
            {"capture_queue_ms", cfg_.capture_queue_ms},
            {"capture_stall_timeout_ms", cfg_.capture_stall_timeout_ms},
            {"aec_tail_ms", cfg_.aec_tail_ms},
            {"barge_in_min_speech_ms", cfg_.barge_in_min_speech_ms},
            {"barge_in_hangover_ms", cfg_.barge_in_hangover_ms},
            {"vad_aggressiveness", cfg_.vad_aggressiveness},
            {"input_device", cfg_.input_device ? json(*cfg_.input_device) : json(nullptr)},
            {"output_device", cfg_.output_device ? json(*cfg_.output_device) : json(nullptr)}
        };
        return {
            {"type", "status"}, {"protocol_version", 2}, {"ready", ready_.load()}, {"aec_ready", ready_.load()},
            {"state", ready_ ? "ready" : "failed"}, {"last_error", last_error},
            {"input_device", input_name_}, {"output_device", output_name_},
            {"device_sample_rate", cfg_.device_sample_rate}, {"capture_sample_rate", cfg_.capture_sample_rate},
            {"render_active", render_active()}, {"aec_metrics", std::move(aec_metrics)},
            {"config", std::move(effective_config)}
        };
    }

    bool restart_requested() const { return restart_requested_.load(); }

    void poll_health() {
        if (!running_.load() || !ready_.load() || restart_requested_.load()) return;
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG last_capture = last_capture_tick_.load();
        if (last_capture != 0 && now - last_capture > static_cast<ULONGLONG>(cfg_.capture_stall_timeout_ms)) {
            request_restart("capture stream stalled for " + std::to_string(now - last_capture) + " ms");
            return;
        }
        const ULONGLONG last_render_progress = last_render_progress_tick_.load();
        if (render_queued_samples_.load() > 0 && last_render_progress != 0 &&
            now - last_render_progress > static_cast<ULONGLONG>(cfg_.capture_stall_timeout_ms)) {
            request_restart("render stream stalled for " + std::to_string(now - last_render_progress) + " ms");
            return;
        }
        if (now - last_device_poll_tick_ < 500) return;
        last_device_poll_tick_ = now;
        if (!device_is_active(capture_device_.Get()) || !device_is_active(render_device_.Get())) {
            request_restart("configured audio endpoint is no longer active");
            return;
        }
        if (!cfg_.input_device && !default_device_matches(eCapture, capture_device_id_)) {
            request_restart("default communication capture endpoint changed");
            return;
        }
        if (!cfg_.output_device && !default_device_matches(eRender, render_device_id_)) {
            request_restart("default communication render endpoint changed");
        }
    }

    uint64_t begin_playback() {
        const uint64_t generation = playback_generation_.fetch_add(1) + 1;
        {
            std::lock_guard lock(render_mutex_);
            render_queue_.clear();
            render_queued_samples_ = 0;
        }
        last_render_tick_.store(0);
        render_cv_.notify_all();
        return generation;
    }

    void enqueue_pcm(const std::string& pcm, int sample_rate, int channels, uint64_t generation) {
        auto samples = resample_pcm16_mono(pcm, sample_rate, channels, cfg_.device_sample_rate);
        size_t offset = 0;
        while (offset < samples.size()) {
            std::unique_lock lock(render_mutex_);
            render_cv_.wait(lock, [&] {
                return !running_ || playback_generation_.load() != generation || render_queue_.size() < max_render_samples_;
            });
            if (!running_) throw std::runtime_error("audio engine is stopped");
            if (playback_generation_.load() != generation) throw std::runtime_error("playback stream was canceled");
            const bool starting_render = render_queue_.empty();
            const size_t available = max_render_samples_ - render_queue_.size();
            const size_t count = std::min(available, samples.size() - offset);
            render_queue_.insert(render_queue_.end(), samples.begin() + offset, samples.begin() + offset + count);
            render_queued_samples_ = render_queue_.size();
            if (starting_render && count > 0) last_render_progress_tick_.store(GetTickCount64());
            offset += count;
            last_render_tick_.store(GetTickCount64());
        }
    }

    bool wait_until_rendered(std::chrono::milliseconds timeout, uint64_t generation) {
        std::unique_lock lock(render_mutex_);
        return render_cv_.wait_for(lock, timeout, [&] {
            return playback_generation_.load() != generation || render_queue_.empty();
        });
    }

    bool cancel_playback(uint64_t expected_generation = 0) {
        if (expected_generation == 0) {
            playback_generation_.fetch_add(1);
        } else {
            uint64_t current = expected_generation;
            if (!playback_generation_.compare_exchange_strong(current, expected_generation + 1)) return false;
        }
        {
            std::lock_guard lock(render_mutex_);
            render_queue_.clear();
            render_queued_samples_ = 0;
        }
        last_render_tick_.store(0);
        render_cv_.notify_all();
        return true;
    }

    bool render_active() const {
        const ULONGLONG last = last_render_tick_.load();
        return last != 0 && GetTickCount64() - last <= static_cast<ULONGLONG>(cfg_.aec_tail_ms);
    }

private:
    void set_last_error(const std::string& message) {
        std::lock_guard lock(error_mutex_);
        last_error_ = message;
    }

    void request_restart(const std::string& message) {
        set_last_error(message);
        ready_ = false;
        restart_requested_ = true;
    }

    void record_device_error(
        const char* operation,
        HRESULT hr,
        std::atomic<uint64_t>& total,
        std::atomic<int>& consecutive
    ) {
        ++total;
        const int failures = consecutive.fetch_add(1) + 1;
        char buffer[160];
        sprintf_s(buffer, "%s failed HRESULT=0x%08lx", operation, static_cast<unsigned long>(hr));
        set_last_error(buffer);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED || failures >= 5) request_restart(buffer);
    }

    static bool device_is_active(IMMDevice* device) {
        if (!device) return false;
        DWORD state = 0;
        return SUCCEEDED(device->GetState(&state)) && (state & DEVICE_STATE_ACTIVE) != 0;
    }

    bool default_device_matches(EDataFlow flow, const std::wstring& expected_id) {
        if (!enumerator_) return false;
        ComPtr<IMMDevice> current;
        if (FAILED(enumerator_->GetDefaultAudioEndpoint(flow, eCommunications, &current))) return false;
        try {
            return device_id(current.Get()) == expected_id;
        } catch (const std::exception&) {
            return false;
        }
    }

    void initialize() {
        check_hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");
        check_hr(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator_)), "MMDeviceEnumerator");
        auto capture = select_device(enumerator_.Get(), eCapture, cfg_.input_device);
        auto render = select_device(enumerator_.Get(), eRender, cfg_.output_device);
        capture_device_ = capture.device;
        render_device_ = render.device;
        capture_device_id_ = capture.id;
        render_device_id_ = render.id;
        input_name_ = capture.name;
        output_name_ = render.name;
        check_hr(capture.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &capture_client_), "activate capture IAudioClient");
        check_hr(render.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &render_client_), "activate render IAudioClient");

        WAVEFORMATEX capture_format{};
        capture_format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        capture_format.nChannels = 1;
        capture_format.nSamplesPerSec = cfg_.device_sample_rate;
        capture_format.wBitsPerSample = 32;
        capture_format.nBlockAlign = 4;
        capture_format.nAvgBytesPerSec = capture_format.nSamplesPerSec * capture_format.nBlockAlign;
        WAVEFORMATEX render_format = capture_format;
        render_format.nChannels = 2;
        render_format.nBlockAlign = 8;
        render_format.nAvgBytesPerSec = render_format.nSamplesPerSec * render_format.nBlockAlign;
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        const REFERENCE_TIME buffer_duration = 100000;
        check_hr(capture_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, buffer_duration, 0, &capture_format, nullptr), "initialize capture");
        check_hr(render_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, buffer_duration, 0, &render_format, nullptr), "initialize render");
        capture_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        render_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!capture_event_ || !render_event_) throw std::runtime_error("CreateEvent failed");
        check_hr(capture_client_->SetEventHandle(capture_event_), "capture SetEventHandle");
        check_hr(render_client_->SetEventHandle(render_event_), "render SetEventHandle");
        check_hr(capture_client_->GetService(IID_PPV_ARGS(&capture_service_)), "IAudioCaptureClient");
        check_hr(render_client_->GetService(IID_PPV_ARGS(&render_service_)), "IAudioRenderClient");
        check_hr(render_client_->GetBufferSize(&render_buffer_frames_), "render GetBufferSize");

        apm_.reset(webrtc::AudioProcessingBuilder().Create());
        if (!apm_) throw std::runtime_error("WebRTC APM creation failed");
        webrtc::AudioProcessing::Config apm_config;
        apm_config.echo_canceller.enabled = true;
        apm_config.high_pass_filter.enabled = true;
        apm_config.noise_suppression.enabled = true;
        apm_config.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::Level::kHigh;
        apm_->ApplyConfig(apm_config);
        if (capture_resampler_.InitializeIfNeeded(cfg_.device_sample_rate, cfg_.capture_sample_rate, 1) != 0) {
            throw std::runtime_error("WebRTC capture resampler initialization failed");
        }
        vad_ = WebRtcVad_Create();
        if (!vad_ || WebRtcVad_Init(vad_) != 0 || WebRtcVad_set_mode(vad_, cfg_.vad_aggressiveness) != 0) {
            throw std::runtime_error("WebRTC VAD initialization failed");
        }
        max_render_samples_ = static_cast<size_t>(cfg_.device_sample_rate * cfg_.render_queue_ms / 1000);
    }

    void process_reverse(const std::vector<float>& frame) {
        const float* source[1] = {frame.data()};
        float* destination[1] = {const_cast<float*>(frame.data())};
        std::lock_guard lock(apm_mutex_);
        const int result = apm_->ProcessReverseStream(source, stream_config_, stream_config_, destination);
        if (result != 0) {
            ++process_errors_;
            set_last_error("WebRTC ProcessReverseStream failed: " + std::to_string(result));
        } else {
            ++reverse_frames_;
        }
    }

    void process_capture(std::vector<float>& frame) {
        const float* source[1] = {frame.data()};
        float* destination[1] = {frame.data()};
        {
            std::lock_guard lock(apm_mutex_);
            const int result = apm_->ProcessStream(source, stream_config_, stream_config_, destination);
            if (result != 0) {
                ++process_errors_;
                set_last_error("WebRTC ProcessStream failed: " + std::to_string(result));
            } else {
                ++capture_frames_;
            }
        }
        std::vector<int16_t> pcm48(frame.size());
        std::transform(frame.begin(), frame.end(), pcm48.begin(), [](float sample) {
            return static_cast<int16_t>(std::clamp(sample, -1.0f, 1.0f) * 32767.0f);
        });
        update_vad(pcm48);
        std::array<int16_t, 160> pcm16{};
        const int output_samples = capture_resampler_.Resample(pcm48.data(), pcm48.size(), pcm16.data(), pcm16.size());
        if (output_samples != static_cast<int>(pcm16.size())) {
            ++process_errors_;
            request_restart("WebRTC capture resampler returned " + std::to_string(output_samples) + " samples");
            return;
        }
        ++resampled_frames_;
        if (binary_callback_) {
            binary_callback_(std::string(reinterpret_cast<const char*>(pcm16.data()), pcm16.size() * sizeof(int16_t)));
        }
    }

    void update_vad(const std::vector<int16_t>& pcm48) {
        const bool speech = WebRtcVad_Process(vad_, cfg_.device_sample_rate, pcm48.data(), pcm48.size()) == 1;
        if (speech) {
            speech_ms_ += cfg_.frame_ms;
            silence_ms_ = 0;
        } else {
            speech_ms_ = 0;
            if (near_end_active_) silence_ms_ += cfg_.frame_ms;
        }
        const bool rendering = render_active();
        if (rendering && !near_end_active_ && speech_ms_ >= cfg_.barge_in_min_speech_ms) {
            near_end_active_ = true;
            ++near_end_events_;
            silence_ms_ = 0;
            speech_id_ = std::to_string(GetTickCount64());
            if (event_callback_) event_callback_({{"type", "near_end_start"}, {"speech_id", speech_id_}, {"render_active", true}, {"ts", GetTickCount64() / 1000.0}});
        }
        if (near_end_active_ && (!rendering || silence_ms_ >= cfg_.barge_in_hangover_ms)) {
            near_end_active_ = false;
            if (event_callback_) event_callback_({{"type", "near_end_end"}, {"speech_id", speech_id_}, {"render_active", rendering}, {"ts", GetTickCount64() / 1000.0}});
            speech_id_.clear();
        }
    }

    void render_loop() {
        DWORD task_index = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
        std::vector<float> reverse_frame;
        reverse_frame.reserve(480);
        while (running_ && !restart_requested_) {
            if (WaitForSingleObject(render_event_, 1000) != WAIT_OBJECT_0) continue;
            UINT32 padding = 0;
            HRESULT hr = render_client_->GetCurrentPadding(&padding);
            if (FAILED(hr)) {
                record_device_error("render GetCurrentPadding", hr, render_device_errors_, consecutive_render_errors_);
                continue;
            }
            consecutive_render_errors_ = 0;
            const UINT32 available = render_buffer_frames_ - padding;
            if (available == 0) continue;
            BYTE* raw = nullptr;
            hr = render_service_->GetBuffer(available, &raw);
            if (FAILED(hr)) {
                record_device_error("render GetBuffer", hr, render_device_errors_, consecutive_render_errors_);
                continue;
            }
            auto* output = reinterpret_cast<float*>(raw);
            bool nonzero = false;
            {
                std::lock_guard lock(render_mutex_);
                for (UINT32 frame = 0; frame < available; ++frame) {
                    float sample = 0.0f;
                    if (!render_queue_.empty()) {
                        sample = render_queue_.front();
                        render_queue_.pop_front();
                    }
                    nonzero = nonzero || std::abs(sample) > 1e-6f;
                    output[frame * 2] = sample;
                    output[frame * 2 + 1] = sample;
                    reverse_frame.push_back(sample);
                    if (reverse_frame.size() == 480) {
                        process_reverse(reverse_frame);
                        reverse_frame.clear();
                    }
                }
                render_queued_samples_ = render_queue_.size();
            }
            if (nonzero) last_render_tick_.store(GetTickCount64());
            hr = render_service_->ReleaseBuffer(available, 0);
            if (FAILED(hr)) {
                record_device_error("render ReleaseBuffer", hr, render_device_errors_, consecutive_render_errors_);
            } else {
                last_render_progress_tick_.store(GetTickCount64());
            }
            render_cv_.notify_all();
        }
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    }

    void capture_loop() {
        DWORD task_index = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
        std::vector<float> capture_frame;
        capture_frame.reserve(480);
        while (running_ && !restart_requested_) {
            if (WaitForSingleObject(capture_event_, 1000) != WAIT_OBJECT_0) continue;
            UINT32 packet = 0;
            HRESULT hr = capture_service_->GetNextPacketSize(&packet);
            if (FAILED(hr)) {
                record_device_error("capture GetNextPacketSize", hr, capture_device_errors_, consecutive_capture_errors_);
                continue;
            }
            consecutive_capture_errors_ = 0;
            while (packet > 0) {
                BYTE* raw = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = capture_service_->GetBuffer(&raw, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) {
                    record_device_error("capture GetBuffer", hr, capture_device_errors_, consecutive_capture_errors_);
                    break;
                }
                const auto* input = reinterpret_cast<const float*>(raw);
                for (UINT32 index = 0; index < frames; ++index) {
                    capture_frame.push_back(flags & AUDCLNT_BUFFERFLAGS_SILENT ? 0.0f : input[index]);
                    if (capture_frame.size() == 480) {
                        process_capture(capture_frame);
                        last_capture_tick_.store(GetTickCount64());
                        capture_frame.clear();
                    }
                }
                hr = capture_service_->ReleaseBuffer(frames);
                if (FAILED(hr)) {
                    record_device_error("capture ReleaseBuffer", hr, capture_device_errors_, consecutive_capture_errors_);
                    break;
                }
                hr = capture_service_->GetNextPacketSize(&packet);
                if (FAILED(hr)) {
                    record_device_error("capture GetNextPacketSize", hr, capture_device_errors_, consecutive_capture_errors_);
                    break;
                }
            }
        }
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    }

    Config cfg_;
    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> restart_requested_{false};
    std::atomic<ULONGLONG> last_render_tick_{0};
    std::atomic<ULONGLONG> last_capture_tick_{0};
    std::atomic<ULONGLONG> last_render_progress_tick_{0};
    std::atomic<size_t> render_queued_samples_{0};
    std::atomic<uint64_t> playback_generation_{0};
    std::atomic<uint64_t> capture_frames_{0};
    std::atomic<uint64_t> reverse_frames_{0};
    std::atomic<uint64_t> process_errors_{0};
    std::atomic<uint64_t> near_end_events_{0};
    std::atomic<uint64_t> resampled_frames_{0};
    std::atomic<uint64_t> capture_device_errors_{0};
    std::atomic<uint64_t> render_device_errors_{0};
    std::atomic<int> consecutive_capture_errors_{0};
    std::atomic<int> consecutive_render_errors_{0};
    ULONGLONG last_device_poll_tick_ = 0;
    std::mutex error_mutex_;
    std::string last_error_;
    std::string input_name_;
    std::string output_name_;
    ComPtr<IAudioClient> capture_client_;
    ComPtr<IAudioClient> render_client_;
    ComPtr<IMMDeviceEnumerator> enumerator_;
    ComPtr<IMMDevice> capture_device_;
    ComPtr<IMMDevice> render_device_;
    std::wstring capture_device_id_;
    std::wstring render_device_id_;
    ComPtr<IAudioCaptureClient> capture_service_;
    ComPtr<IAudioRenderClient> render_service_;
    HANDLE capture_event_ = nullptr;
    HANDLE render_event_ = nullptr;
    UINT32 render_buffer_frames_ = 0;
    std::thread capture_thread_;
    std::thread render_thread_;
    std::mutex render_mutex_;
    std::condition_variable render_cv_;
    std::deque<float> render_queue_;
    size_t max_render_samples_ = 24000;
    std::unique_ptr<webrtc::AudioProcessing> apm_;
    webrtc::PushResampler<int16_t> capture_resampler_;
    webrtc::StreamConfig stream_config_{48000, 1};
    std::mutex apm_mutex_;
    VadInst* vad_ = nullptr;
    int speech_ms_ = 0;
    int silence_ms_ = 0;
    bool near_end_active_ = false;
    std::string speech_id_;
    BinaryCallback binary_callback_;
    EventCallback event_callback_;
};

struct ClientState {
    int sample_rate = 0;
    int channels = 0;
    size_t expected_bytes = 0;
    std::string chunk_request_id;
    std::string stream_id;
    uint64_t playback_generation = 0;
    bool playback_active = false;
};

struct CaptureTransportItem {
    std::string payload;
    bool binary = false;
};

class SpscCaptureQueue {
public:
    explicit SpscCaptureQueue(size_t capacity) : slots_(std::max<size_t>(2, capacity + 1)) {
        for (auto& slot : slots_) slot.payload.reserve(512);
    }

    bool try_push(std::string payload, bool binary) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = increment(head);
        if (next == tail_.load(std::memory_order_acquire)) return false;
        slots_[head].payload = std::move(payload);
        slots_[head].binary = binary;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool try_pop(CaptureTransportItem& item) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        item.payload.swap(slots_[tail].payload);
        item.binary = slots_[tail].binary;
        tail_.store(increment(tail), std::memory_order_release);
        return true;
    }

    bool empty() const {
        return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
    }

    size_t approximate_size() const {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_acquire);
        return head >= tail ? head - tail : slots_.size() - tail + head;
    }

private:
    size_t increment(size_t value) const { return (value + 1) % slots_.size(); }

    std::vector<CaptureTransportItem> slots_;
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
};

class ProtocolServer {
public:
    ProtocolServer(Config cfg, AudioEngine& engine)
        : cfg_(std::move(cfg)),
          engine_(engine),
          server_(cfg_.port, cfg_.host),
          capture_transport_queue_(static_cast<size_t>(std::max(2, cfg_.capture_queue_ms / cfg_.frame_ms))) {
        max_capture_buffer_bytes_ = static_cast<size_t>(cfg_.capture_sample_rate * sizeof(int16_t) * cfg_.capture_queue_ms / 1000);
        capture_drop_threshold_ = static_cast<size_t>(std::max(1, cfg_.capture_queue_ms / cfg_.frame_ms));
        engine_.set_callbacks(
            [this](std::string pcm) { enqueue_capture_transport(std::move(pcm), true); },
            [this](const json& event) { enqueue_capture_transport(event.dump(), false); }
        );
    }

    bool start() {
        server_.setOnClientMessageCallback([this](std::shared_ptr<ix::ConnectionState> state, ix::WebSocket& ws, const ix::WebSocketMessagePtr& message) {
            handle_message(state, ws, message);
        });
        server_.disablePerMessageDeflate();
        const auto result = server_.listen();
        if (!result.first) {
            std::cerr << "WebSocket listen failed: " << result.second << std::endl;
            return false;
        }
        transport_running_ = true;
        transport_thread_ = std::thread(&ProtocolServer::transport_loop, this);
        server_.start();
        return true;
    }

    void stop() {
        if (transport_running_.exchange(false)) {
            transport_cv_.notify_all();
            if (transport_thread_.joinable()) transport_thread_.join();
        }
        server_.stop();
    }
    bool shutdown_requested() const { return shutdown_requested_.load(); }

private:
    void enqueue_capture_transport(std::string payload, bool binary) {
        if (!capture_transport_queue_.try_push(std::move(payload), binary)) {
            ++capture_ingress_drops_;
            const size_t streak = capture_ingress_drop_streak_.fetch_add(1) + 1;
            if (streak >= capture_drop_threshold_) capture_ingress_healthy_ = false;
            return;
        }
        capture_ingress_drop_streak_ = 0;
        capture_ingress_healthy_ = true;
        transport_cv_.notify_one();
    }

    void transport_loop() {
        CaptureTransportItem item;
        item.payload.reserve(512);
        while (transport_running_.load() || !capture_transport_queue_.empty()) {
            if (capture_transport_queue_.try_pop(item)) {
                if (item.binary) {
                    broadcast_binary(item.payload);
                } else {
                    try {
                        broadcast_json(json::parse(item.payload));
                    } catch (const std::exception&) {
                        ++capture_ingress_drops_;
                    }
                }
                continue;
            }
            std::unique_lock lock(transport_wait_mutex_);
            transport_cv_.wait_for(lock, 10ms, [this] {
                return !transport_running_.load() || !capture_transport_queue_.empty();
            });
        }
    }

    static json response(const json& request, const std::string& type, bool ok = true) {
        json result = {{"type", type}, {"ok", ok}};
        if (request.contains("request_id")) result["request_id"] = request["request_id"];
        if (request.contains("type")) result["cmd"] = request["type"];
        return result;
    }

    void handle_message(const std::shared_ptr<ix::ConnectionState>& state, ix::WebSocket& ws, const ix::WebSocketMessagePtr& message) {
        const std::string id = state->getId();
        if (message->type == ix::WebSocketMessageType::Open) {
            if (message->openInfo.uri != cfg_.ws_path) {
                ws.send(json({{"type", "error"}, {"code", "BAD_PATH"}, {"message", "unsupported path"}}).dump());
                ws.close();
                return;
            }
            {
                std::lock_guard lock(clients_mutex_);
                clients_.insert(&ws);
                states_[id] = {};
                capture_drop_streaks_[&ws] = 0;
                capture_enabled_[&ws] = false;
            }
            ws.send(status_message().dump());
            {
                std::lock_guard lock(clients_mutex_);
                const auto existing = capture_enabled_.find(&ws);
                if (existing != capture_enabled_.end()) existing->second = true;
            }
            return;
        }
        if (message->type == ix::WebSocketMessageType::Close) {
            bool cancel_orphaned_playback = false;
            uint64_t orphaned_generation = 0;
            {
                std::lock_guard lock(clients_mutex_);
                clients_.erase(&ws);
                capture_drop_streaks_.erase(&ws);
                capture_enabled_.erase(&ws);
                const auto existing = states_.find(id);
                cancel_orphaned_playback = existing != states_.end() && existing->second.playback_active;
                if (cancel_orphaned_playback) orphaned_generation = existing->second.playback_generation;
                states_.erase(id);
                refresh_capture_health_locked();
            }
            if (cancel_orphaned_playback && engine_.cancel_playback(orphaned_generation)) {
                broadcast_json({{"type", "playback_ended"}});
            }
            return;
        }
        if (message->type != ix::WebSocketMessageType::Message) return;
        try {
            if (message->binary) {
                handle_binary(id, ws, message->str);
            } else {
                handle_json(id, ws, json::parse(message->str));
            }
        } catch (const std::exception& exc) {
            json error = {{"type", "error"}, {"ok", false}, {"message", exc.what()}};
            if (message->binary) {
                std::lock_guard lock(clients_mutex_);
                const auto existing = states_.find(id);
                if (existing != states_.end()) {
                    if (!existing->second.chunk_request_id.empty()) error["request_id"] = existing->second.chunk_request_id;
                    existing->second.expected_bytes = 0;
                    existing->second.chunk_request_id.clear();
                }
            }
            ws.send(error.dump());
        }
    }

    void handle_binary(const std::string& id, ix::WebSocket& ws, const std::string& data) {
        ClientState state;
        {
            std::lock_guard lock(clients_mutex_);
            state = states_.at(id);
        }
        if (!state.playback_active || state.expected_bytes == 0 || data.size() != state.expected_bytes) {
            throw std::runtime_error("binary PCM does not match an active play_chunk declaration");
        }
        engine_.enqueue_pcm(data, state.sample_rate, state.channels, state.playback_generation);
        {
            std::lock_guard lock(clients_mutex_);
            const auto existing = states_.find(id);
            if (existing != states_.end()) {
                existing->second.expected_bytes = 0;
                existing->second.chunk_request_id.clear();
            }
        }
        json request = {{"type", "play_chunk"}, {"request_id", state.chunk_request_id}};
        ws.send(response(request, "ack").dump());
    }

    void handle_json(const std::string& id, ix::WebSocket& ws, const json& request) {
        const std::string type = request.value("type", "");
        if (type == "status") {
            json status = status_message();
            if (request.contains("request_id")) status["request_id"] = request["request_id"];
            ws.send(status.dump());
        } else if (type == "ping") {
            json pong = status_message();
            pong["type"] = "pong";
            pong["ok"] = true;
            if (request.contains("request_id")) pong["request_id"] = request["request_id"];
            ws.send(pong.dump());
        } else if (type == "play_start") {
            const int sample_rate = request.value("sample_rate", 0);
            const int channels = request.value("channels", 0);
            const std::string stream_id = request.value("stream_id", "");
            if (stream_id.empty()) throw std::runtime_error("play_start stream_id must not be empty");
            if (sample_rate <= 0 || channels < 1 || channels > 2) throw std::runtime_error("unsupported playback format");
            const uint64_t generation = engine_.begin_playback();
            {
                std::lock_guard lock(clients_mutex_);
                ClientState& state = states_.at(id);
                state.sample_rate = sample_rate;
                state.channels = channels;
                state.stream_id = stream_id;
                state.playback_generation = generation;
                state.playback_active = true;
            }
            ws.send(response(request, "ack").dump());
            broadcast_json({{"type", "playback_started"}, {"stream_id", stream_id}});
        } else if (type == "play_chunk") {
            const size_t expected_bytes = request.value("bytes", 0U);
            const std::string stream_id = request.value("stream_id", "");
            if (expected_bytes == 0) throw std::runtime_error("play_chunk bytes must be positive");
            {
                std::lock_guard lock(clients_mutex_);
                ClientState& state = states_.at(id);
                if (!state.playback_active || stream_id.empty() || stream_id != state.stream_id) {
                    throw std::runtime_error("play_chunk does not match the active playback stream");
                }
                if (state.expected_bytes != 0) throw std::runtime_error("previous play_chunk is still awaiting binary PCM");
                state.expected_bytes = expected_bytes;
                state.chunk_request_id = request.value("request_id", "");
            }
        } else if (type == "play_end") {
            const std::string stream_id = request.value("stream_id", "");
            uint64_t generation = 0;
            {
                std::lock_guard lock(clients_mutex_);
                const ClientState& state = states_.at(id);
                if (!state.playback_active || stream_id.empty() || stream_id != state.stream_id) {
                    throw std::runtime_error("play_end does not match the active playback stream");
                }
                if (state.expected_bytes != 0) throw std::runtime_error("play_end received while PCM binary is pending");
                generation = state.playback_generation;
            }
            const bool drained = engine_.wait_until_rendered(5s, generation);
            if (!drained) throw std::runtime_error("render drain timeout");
            {
                std::lock_guard lock(clients_mutex_);
                states_.at(id).playback_active = false;
            }
            ws.send(response(request, "ack").dump());
            broadcast_json({{"type", "playback_ended"}, {"stream_id", stream_id}});
        } else if (type == "play_cancel") {
            engine_.cancel_playback();
            {
                std::lock_guard lock(clients_mutex_);
                for (auto& [_, state] : states_) state.playback_active = false;
            }
            ws.send(response(request, "ack").dump());
            broadcast_json({{"type", "playback_ended"}});
        } else if (type == "shutdown") {
            ws.send(response(request, "ack").dump());
            shutdown_requested_ = true;
        } else {
            json error = response(request, "error", false);
            error["code"] = "UNSUPPORTED_COMMAND";
            error["message"] = "unsupported command: " + type;
            ws.send(error.dump());
        }
    }

    void broadcast_json(const json& data) {
        const std::string payload = data.dump();
        std::lock_guard lock(clients_mutex_);
        for (auto* client : clients_) client->send(payload);
    }

    void broadcast_binary(const std::string& data) {
        std::lock_guard lock(clients_mutex_);
        for (auto* client : clients_) {
            if (!capture_enabled_[client]) continue;
            size_t& drop_streak = capture_drop_streaks_[client];
            if (client->bufferedAmount() + data.size() > max_capture_buffer_bytes_) {
                ++capture_drops_;
                ++drop_streak;
                continue;
            }
            client->sendBinary(data);
            drop_streak = 0;
        }
        refresh_capture_health_locked();
    }

    json status_message() {
        json status = engine_.status();
        const bool capture_healthy = capture_transport_healthy_.load() && capture_ingress_healthy_.load();
        status["capture_transport_ready"] = capture_healthy;
        status["capture_drops"] = capture_drops_.load();
        status["capture_ingress_drops"] = capture_ingress_drops_.load();
        status["capture_ingress_queue_depth"] = capture_transport_queue_.approximate_size();
        if (!capture_healthy) {
            status["ready"] = false;
            status["last_error"] = "capture transport backpressure exceeded the configured threshold";
        }
        return status;
    }

    void refresh_capture_health_locked() {
        bool healthy = true;
        for (const auto& [_, streak] : capture_drop_streaks_) {
            if (streak >= capture_drop_threshold_) {
                healthy = false;
                break;
            }
        }
        capture_transport_healthy_ = healthy;
    }

    Config cfg_;
    AudioEngine& engine_;
    ix::WebSocketServer server_;
    std::atomic<bool> shutdown_requested_{false};
    std::mutex clients_mutex_;
    std::set<ix::WebSocket*> clients_;
    std::unordered_map<std::string, ClientState> states_;
    std::unordered_map<ix::WebSocket*, size_t> capture_drop_streaks_;
    std::unordered_map<ix::WebSocket*, bool> capture_enabled_;
    size_t max_capture_buffer_bytes_ = 32000;
    size_t capture_drop_threshold_ = 100;
    std::atomic<uint64_t> capture_drops_{0};
    std::atomic<bool> capture_transport_healthy_{true};
    SpscCaptureQueue capture_transport_queue_;
    std::atomic<bool> transport_running_{false};
    std::thread transport_thread_;
    std::mutex transport_wait_mutex_;
    std::condition_variable transport_cv_;
    std::atomic<uint64_t> capture_ingress_drops_{0};
    std::atomic<size_t> capture_ingress_drop_streak_{0};
    std::atomic<bool> capture_ingress_healthy_{true};
};

std::atomic<bool> interrupted{false};
BOOL WINAPI console_handler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
        interrupted = true;
        return TRUE;
    }
    return FALSE;
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path = "configs/config.json";
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == "--config" && index + 1 < argc) config_path = argv[++index];
    }
    try {
        const Config cfg = load_config(config_path);
        SetConsoleCtrlHandler(console_handler, TRUE);
        ix::initNetSystem();
        AudioEngine engine(cfg);
        ProtocolServer server(cfg, engine);
        const bool audio_ready = engine.start();
        if (!audio_ready) std::cerr << "Audio/AEC initialization failed; voice sessions will be rejected" << std::endl;
        if (!server.start()) return 2;
        std::cout << "AudioRuntime listening on ws://" << cfg.host << ':' << cfg.port << cfg.ws_path << std::endl;
        while (!interrupted && !server.shutdown_requested() && !engine.restart_requested()) {
            engine.poll_health();
            std::this_thread::sleep_for(100ms);
        }
        const bool restart_requested = engine.restart_requested();
        if (restart_requested) std::cerr << "AudioRuntime requested a clean restart after an audio device failure" << std::endl;
        engine.stop();
        server.stop();
        ix::uninitNetSystem();
        return restart_requested ? 4 : (audio_ready ? 0 : 3);
    } catch (const std::exception& exc) {
        std::cerr << "AudioRuntime fatal error: " << exc.what() << std::endl;
        return 1;
    }
}
