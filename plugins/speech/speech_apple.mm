// speech_apple.mm - macOS and iOS: AVSpeechSynthesizer's write (macOS 10.15, iOS 13), its
// buffers collected and written as a 16-bit WAV file when the last one comes
//
// SPDX-License-Identifier: 0BSD
#include "backend.h"

#if defined(__APPLE__)

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>

namespace speech {

namespace {

// What one request's buffers fill, shared with the callback (which may run on another thread)
struct Job {
	std::mutex lock;
	std::vector<short> samples;
	int rate = 0, channels = 1;
	std::atomic<int> state{ int(State::Busy) };
	std::atomic<bool> cancelled{ false };
	std::string file, error;
};

std::string utf8(NSString *s) { return s ? std::string([s UTF8String]) : std::string(); }

class AppleBackend : public Backend {
	AVSpeechSynthesizer *synth = nil;
	std::shared_ptr<Job> job;

public:
	AppleBackend() {
		if (@available(macOS 10.15, iOS 13.0, *)) synth = [[AVSpeechSynthesizer alloc] init];
	}
	~AppleBackend() override { cancel(); }

	bool available() override {
		if (@available(macOS 10.15, iOS 13.0, *)) return synth != nil && [AVSpeechSynthesisVoice speechVoices].count > 0;
		return false;
	}

	std::vector<Voice> voices() override {
		std::vector<Voice> out;
		for (AVSpeechSynthesisVoice *v in [AVSpeechSynthesisVoice speechVoices]) {
			Voice voice;
			voice.id = utf8(v.identifier);
			voice.name = utf8(v.name);
			voice.language = utf8(v.language);
			out.push_back(voice);
		}
		return out;
	}

	bool start(const Request &r, std::string &err) override {
		cancel();
		if (@available(macOS 10.15, iOS 13.0, *)) {
			if (!synth) {
				err = "no speech synthesizer";
				return false;
			}
			if (r.file.empty()) {
				err = "no file to speak into";
				return false;
			}
			NSString *text = [NSString stringWithUTF8String:r.text.c_str()];
			if (!text) {
				err = "the text isn't UTF-8";
				return false;
			}
			AVSpeechUtterance *u = [AVSpeechUtterance speechUtteranceWithString:text];
			if (!r.voice.empty()) {
				AVSpeechSynthesisVoice *v = [AVSpeechSynthesisVoice voiceWithIdentifier:[NSString stringWithUTF8String:r.voice.c_str()]];
				if (!v) v = [AVSpeechSynthesisVoice voiceWithLanguage:[NSString stringWithUTF8String:r.voice.c_str()]];
				if (v) u.voice = v;
			}
			float rate = float(AVSpeechUtteranceDefaultSpeechRate * clampd(r.rate, 10, 400) / 100.0);
			u.rate = std::fmax(AVSpeechUtteranceMinimumSpeechRate, std::fmin(AVSpeechUtteranceMaximumSpeechRate, rate));
			u.pitchMultiplier = float(clampd(r.pitch / 100.0, 0.5, 2.0));
			u.volume = float(clampd(r.volume / 100.0, 0, 1));

			auto j = std::make_shared<Job>();
			j->file = r.file;
			job = j;
			[synth writeUtterance:u
			     toBufferCallback:^(AVAudioBuffer *buffer) {
				     if (j->cancelled) return;
				     AVAudioPCMBuffer *pcm = [buffer isKindOfClass:[AVAudioPCMBuffer class]] ? (AVAudioPCMBuffer *)buffer : nil;
				     if (!pcm || pcm.frameLength == 0) {
					     // the last one: write the file
					     std::lock_guard<std::mutex> hold(j->lock);
					     if (j->samples.empty() || j->rate <= 0) {
						     j->error = "the synthesizer gave no sound";
						     j->state = int(State::Failed);
					     } else if (!writeWav(j->file, j->samples, j->rate, j->channels)) {
						     j->error = "can't write " + j->file;
						     j->state = int(State::Failed);
					     } else {
						     j->state = int(State::Done);
					     }
					     return;
				     }
				     std::lock_guard<std::mutex> hold(j->lock);
				     AVAudioFormat *fmt = pcm.format;
				     j->rate = int(fmt.sampleRate);
				     j->channels = 1; // the first channel: speech is mono
				     AVAudioFrameCount n = pcm.frameLength;
				     if (pcm.floatChannelData) {
					     const float *src = pcm.floatChannelData[0];
					     for (AVAudioFrameCount k = 0; k < n; k++)
						     j->samples.push_back(short(std::lround(clampd(src[k], -1, 1) * 32767)));
				     } else if (pcm.int16ChannelData) {
					     const int16_t *src = pcm.int16ChannelData[0];
					     size_t stride = fmt.isInterleaved ? size_t(fmt.channelCount) : 1;
					     for (AVAudioFrameCount k = 0; k < n; k++) j->samples.push_back(src[k * stride]);
				     } else if (pcm.int32ChannelData) {
					     const int32_t *src = pcm.int32ChannelData[0];
					     size_t stride = fmt.isInterleaved ? size_t(fmt.channelCount) : 1;
					     for (AVAudioFrameCount k = 0; k < n; k++) j->samples.push_back(short(src[k * stride] >> 16));
				     }
			     }];
			return true;
		}
		err = "needs macOS 10.15 or iOS 13";
		return false;
	}

	State poll(std::string &err) override {
		if (!job) return State::Idle;
		std::lock_guard<std::mutex> hold(job->lock);
		err = job->error;
		return State(job->state.load());
	}

	void cancel() override {
		if (job) {
			job->cancelled = true;
			[synth stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
			job.reset();
		}
	}
};

} // namespace

Backend *makeBackend() { return new AppleBackend(); }

} // namespace speech

#endif
