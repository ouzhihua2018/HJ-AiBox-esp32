#include "passthrough_audio_processor.h"

void PassthroughAudioProcessor::Initialize(AudioCodec* codec) {
    codec_ = codec;
}

void PassthroughAudioProcessor::Feed(const std::vector<int16_t>& data) {
    if (!running_ || !output_callback_) {
        return;
    }
    std::vector<int16_t> copy(data);
    output_callback_(std::move(copy));
}

void PassthroughAudioProcessor::Start() {
    running_ = true;
}

void PassthroughAudioProcessor::Stop() {
    running_ = false;
}

bool PassthroughAudioProcessor::IsRunning() {
    return running_;
}

void PassthroughAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = std::move(callback);
}

void PassthroughAudioProcessor::OnVadStateChange(std::function<void(bool speaking)> callback) {
    (void)callback;
}

size_t PassthroughAudioProcessor::GetFeedSize() {
    // One Opus frame at 16 kHz (application resamples DAA PCM to this rate).
    return 16000 * 60 / 1000;
}

void PassthroughAudioProcessor::EnableDeviceAec(bool enable) {
    (void)enable;
}
