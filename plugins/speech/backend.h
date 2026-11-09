// backend.h - the system's speech, one backend per platform
//
//   Windows   SAPI 5: a voice spoken into a WAV file (speech_windows.cpp)
//   macOS,    AVSpeechSynthesizer's write (macOS 10.15, iOS 13), its buffers put into a WAV
//   iOS       file (speech_apple.mm)
//   Linux     eSpeak NG (espeak-ng, or espeak) when it's installed, run into a WAV file
//             (speech_posix.cpp)
//   Android   android.speech.tts.TextToSpeech's synthesizeToFile, through JNI (speech_android.cpp)
//   the web   the browser's speechSynthesis, which speaks aloud itself: "direct" mode
//             (speech_web.cpp)
//
// One line at a time: a new start cancels the one before.
//
// SPDX-License-Identifier: 0BSD
#pragma once

#include <string>
#include <vector>

namespace speech {

struct Voice {
	std::string id;        ///< what a request names it by
	std::string name;      ///< for people
	std::string language;  ///< a BCP 47 tag where the system gives one ("en-US", "ja-JP")
};

struct Request {
	std::string text;      ///< UTF-8
	std::string voice;     ///< a Voice's id; empty for the system's default
	double rate = 100;     ///< % of the voice's normal speed
	double pitch = 100;    ///< % of its normal pitch
	double volume = 100;   ///< %
	std::string file;      ///< "file" mode: the WAV file to make (a native path)
};

enum class State { Idle, Busy, Done, Failed };

class Backend {
public:
	virtual ~Backend() {}
	/// Whether there's a speech engine to use
	virtual bool available() = 0;
	/// "file": a request makes a WAV file the game plays; "direct": the system speaks it aloud
	virtual const char *mode() { return "file"; }
	/// The voices (may be empty until the engine is ready, on Android and the web)
	virtual std::vector<Voice> voices() = 0;
	/// Starts a request; false (with error) when it can't
	virtual bool start(const Request &r, std::string &error) = 0;
	/// Where the request is
	virtual State poll(std::string &error) = 0;
	virtual void cancel() = 0;
};

/// This platform's backend
Backend *makeBackend();

// Helpers the backends share (common.cpp)

/// Writes a 16-bit PCM WAV file
bool writeWav(const std::string &path, const std::vector<short> &samples, int rate, int channels);
/// Text escaped for XML
std::string xmlEscape(const std::string &s);
/// Clamps
double clampd(double v, double lo, double hi);

} // namespace speech
