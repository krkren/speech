// speech_web.cpp - the web: the browser's speechSynthesis, which speaks aloud itself ("direct"
// mode: there's no file, and the browser's own volume applies, not the game's)
//
// SPDX-License-Identifier: 0BSD
#include "backend.h"

#if defined(__EMSCRIPTEN__)

#include <emscripten.h>

#include <sstream>

// clang-format off
EM_JS(int, speech_web_available, (), {
	return (typeof window !== "undefined" && window.speechSynthesis && typeof SpeechSynthesisUtterance !== "undefined") ? 1 : 0;
});

// the voices as "id\tname\tlang\n" lines, into buf; the length it needs
EM_JS(int, speech_web_voices, (char *buf, int size), {
	var s = "";
	try {
		var vs = window.speechSynthesis.getVoices() || [];
		for (var k = 0; k < vs.length; k++) {
			var v = vs[k];
			s += String(v.voiceURI).replace(/[\t\n]/g, " ") + "\t" + String(v.name).replace(/[\t\n]/g, " ") + "\t" + String(v.lang || "") + "\n";
		}
	} catch (e) {}
	var need = lengthBytesUTF8(s) + 1;
	if (buf && size >= need) stringToUTF8(s, buf, size);
	return need;
});

EM_JS(int, speech_web_speak, (const char *text, const char *voice, double rate, double pitch, double volume), {
	try {
		var synth = window.speechSynthesis;
		synth.cancel();
		var u = new SpeechSynthesisUtterance(UTF8ToString(text));
		var id = UTF8ToString(voice);
		if (id) {
			var vs = synth.getVoices() || [];
			for (var k = 0; k < vs.length; k++) if (vs[k].voiceURI == id || vs[k].name == id || vs[k].lang == id) { u.voice = vs[k]; break; }
		}
		u.rate = Math.max(0.1, Math.min(10, rate / 100));
		u.pitch = Math.max(0, Math.min(2, pitch / 100));
		u.volume = Math.max(0, Math.min(1, volume / 100));
		Module.kagexSpeechState = 1;
		Module.kagexSpeechError = "";
		u.onend = function() { if (Module.kagexSpeechUtterance === u) Module.kagexSpeechState = 2; };
		u.onerror = function(ev) { if (Module.kagexSpeechUtterance === u) { Module.kagexSpeechState = 3; Module.kagexSpeechError = String(ev && ev.error || "error"); } };
		Module.kagexSpeechUtterance = u;
		synth.speak(u);
		return 1;
	} catch (e) {
		Module.kagexSpeechState = 3;
		Module.kagexSpeechError = String(e);
		return 0;
	}
});

EM_JS(int, speech_web_state, (), { return Module.kagexSpeechState || 0; });

EM_JS(int, speech_web_error, (char *buf, int size), {
	var s = Module.kagexSpeechError || "";
	var need = lengthBytesUTF8(s) + 1;
	if (buf && size >= need) stringToUTF8(s, buf, size);
	return need;
});

EM_JS(void, speech_web_cancel, (), {
	try { Module.kagexSpeechUtterance = null; Module.kagexSpeechState = 0; window.speechSynthesis.cancel(); } catch (e) {}
});
// clang-format on

namespace speech {

namespace {

std::string fetch(int (*f)(char *, int)) {
	int need = f(nullptr, 0);
	std::string s(size_t(need), '\0');
	f(&s[0], need);
	if (!s.empty()) s.resize(s.size() - 1);
	return s;
}

class WebBackend : public Backend {
public:
	bool available() override { return speech_web_available() != 0; }
	const char *mode() override { return "direct"; }

	std::vector<Voice> voices() override {
		std::vector<Voice> out;
		if (!available()) return out;
		std::istringstream in(fetch(speech_web_voices));
		std::string line;
		while (std::getline(in, line)) {
			Voice v;
			size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
			if (b == std::string::npos) continue;
			v.id = line.substr(0, a);
			v.name = line.substr(a + 1, b - a - 1);
			v.language = line.substr(b + 1);
			out.push_back(v);
		}
		return out;
	}

	bool start(const Request &r, std::string &err) override {
		if (!available()) {
			err = "no speechSynthesis in this browser";
			return false;
		}
		if (!speech_web_speak(r.text.c_str(), r.voice.c_str(), r.rate, r.pitch, r.volume)) {
			err = fetch(speech_web_error);
			return false;
		}
		return true;
	}

	State poll(std::string &err) override {
		int s = speech_web_state();
		if (s == 3) err = fetch(speech_web_error);
		return s == 1 ? State::Busy : s == 2 ? State::Done : s == 3 ? State::Failed : State::Idle;
	}

	void cancel() override { speech_web_cancel(); }
};

} // namespace

Backend *makeBackend() { return new WebBackend(); }

} // namespace speech

#endif
