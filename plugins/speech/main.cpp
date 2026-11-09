// main.cpp - the speech plugin: Speech, the system's text to speech for scripts
//
//   Speech.available         whether the system has a speech engine the plugin can use
//   Speech.mode              "file": speak() makes a WAV file the game plays (as a voice, at
//                            the voice volume); "direct": the system speaks aloud itself (the web)
//   Speech.voices()          the voices: an Array of %[id, name, language]; may be empty until
//                            the engine is ready (Android, the web)
//   Speech.speak(text, %[voice, rate, pitch, volume, file])
//                            starts a line (rate, pitch, volume in %, 100 as the voice is; in
//                            "file" mode `file` is the WAV to make, a native path); true when it
//                            started. One line at a time: a new one cancels the one before
//   Speech.state             "idle", "busy", "done" or "failed"
//   Speech.cancel()
//   Speech.lastError         why the last line failed (also in the log)
//
// SPDX-License-Identifier: 0BSD

#include "ncbind.hpp"

#include "backend.h"

#include <memory>
#include <string>

namespace {

std::string toUtf8(const ttstr &s) {
	const tjs_char *p = s.c_str();
	size_t len = size_t(s.length());
	std::string out;
	out.reserve(len);
	for (size_t k = 0; k < len; k++) {
		uint32_t c = uint16_t(p[k]);
		if (c >= 0xD800 && c < 0xDC00 && k + 1 < len && uint16_t(p[k + 1]) >= 0xDC00 && uint16_t(p[k + 1]) < 0xE000) {
			c = 0x10000 + ((c - 0xD800) << 10) + (uint16_t(p[k + 1]) - 0xDC00);
			k++;
		} else if (c >= 0xD800 && c < 0xE000) {
			c = 0xFFFD;
		}
		if (c < 0x80) {
			out.push_back(char(c));
		} else if (c < 0x800) {
			out.push_back(char(0xC0 | (c >> 6)));
			out.push_back(char(0x80 | (c & 0x3F)));
		} else if (c < 0x10000) {
			out.push_back(char(0xE0 | (c >> 12)));
			out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
			out.push_back(char(0x80 | (c & 0x3F)));
		} else {
			out.push_back(char(0xF0 | (c >> 18)));
			out.push_back(char(0x80 | ((c >> 12) & 0x3F)));
			out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
			out.push_back(char(0x80 | (c & 0x3F)));
		}
	}
	return out;
}

ttstr fromUtf8(const std::string &s) {
	std::u16string out;
	const unsigned char *p = reinterpret_cast<const unsigned char *>(s.c_str());
	while (*p) {
		uint32_t c = *p++;
		int more = 0;
		if (c >= 0xF0) c &= 0x07, more = 3;
		else if (c >= 0xE0) c &= 0x0F, more = 2;
		else if (c >= 0xC0) c &= 0x1F, more = 1;
		for (; more > 0 && (*p & 0xC0) == 0x80; more--) c = (c << 6) | (*p++ & 0x3F);
		if (c >= 0x10000) {
			c -= 0x10000;
			out.push_back(char16_t(0xD800 + (c >> 10)));
			out.push_back(char16_t(0xDC00 + (c & 0x3FF)));
		} else {
			out.push_back(char16_t(c));
		}
	}
	std::vector<tjs_char> buf(out.begin(), out.end());
	buf.push_back(0);
	return ttstr(buf.data());
}

std::unique_ptr<speech::Backend> backend;
std::string lastErrorText;

speech::Backend &the() {
	if (!backend) backend.reset(speech::makeBackend());
	return *backend;
}

void setDict(iTJSDispatch2 *d, const tjs_char *name, const std::string &v) {
	tTJSVariant val(fromUtf8(v));
	d->PropSet(TJS_MEMBERENSURE, name, nullptr, &val, d);
}

bool dictValue(iTJSDispatch2 *d, const tjs_char *name, tTJSVariant &out) {
	return d && TJS_SUCCEEDED(d->PropGet(0, name, nullptr, &out, d)) && out.Type() != tvtVoid;
}

} // namespace

class Speech {
public:
	static bool getAvailable() { return the().available(); }
	static ttstr getMode() { return ttstr(the().mode()); }
	static ttstr getLastError() { return fromUtf8(lastErrorText); }

	static ttstr getState() {
		std::string err;
		speech::State s = the().poll(err);
		if (s == speech::State::Failed && err != lastErrorText) {
			lastErrorText = err;
			TVPAddLog(ttstr(TJS_W("speech: ")) + fromUtf8(err));
		}
		switch (s) {
		case speech::State::Busy: return TJS_W("busy");
		case speech::State::Done: return TJS_W("done");
		case speech::State::Failed: return TJS_W("failed");
		default: return TJS_W("idle");
		}
	}

	static tjs_error TJS_INTF_METHOD voices(tTJSVariant *result, tjs_int, tTJSVariant **, iTJSDispatch2 *) {
		std::vector<speech::Voice> list = the().voices();
		iTJSDispatch2 *arr = TJSCreateArrayObject();
		for (size_t k = 0; k < list.size(); k++) {
			iTJSDispatch2 *d = TJSCreateDictionaryObject();
			setDict(d, TJS_W("id"), list[k].id);
			setDict(d, TJS_W("name"), list[k].name);
			setDict(d, TJS_W("language"), list[k].language);
			tTJSVariant v(d, d);
			d->Release();
			arr->PropSetByNum(TJS_MEMBERENSURE, tjs_int(k), &v, arr);
		}
		if (result) *result = tTJSVariant(arr, arr);
		arr->Release();
		return TJS_S_OK;
	}

	static tjs_error TJS_INTF_METHOD speak(tTJSVariant *result, tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *) {
		speech::Request r;
		if (numparams >= 1 && param[0]->Type() != tvtVoid) r.text = toUtf8(ttstr(*param[0]));
		iTJSDispatch2 *opts = numparams >= 2 && param[1]->Type() == tvtObject ? param[1]->AsObjectNoAddRef() : nullptr;
		tTJSVariant v;
		if (dictValue(opts, TJS_W("voice"), v)) r.voice = toUtf8(ttstr(v));
		if (dictValue(opts, TJS_W("rate"), v)) r.rate = double(tjs_real(v));
		if (dictValue(opts, TJS_W("pitch"), v)) r.pitch = double(tjs_real(v));
		if (dictValue(opts, TJS_W("volume"), v)) r.volume = double(tjs_real(v));
		if (dictValue(opts, TJS_W("file"), v)) r.file = toUtf8(ttstr(v));
		std::string err;
		bool ok = !r.text.empty() && the().start(r, err);
		if (r.text.empty()) err = "nothing to say";
		if (!ok) {
			lastErrorText = err;
			TVPAddLog(ttstr(TJS_W("speech: ")) + fromUtf8(err));
		} else {
			lastErrorText.clear();
		}
		if (result) *result = ok;
		return TJS_S_OK;
	}

	static void cancel() { the().cancel(); }
};

NCB_REGISTER_CLASS(Speech) {
	NCB_PROPERTY_RO(available, getAvailable);
	NCB_PROPERTY_RO(mode, getMode);
	NCB_PROPERTY_RO(state, getState);
	NCB_PROPERTY_RO(lastError, getLastError);
	NCB_METHOD_RAW_CALLBACK(voices, &Speech::voices, TJS_STATICMEMBER);
	NCB_METHOD_RAW_CALLBACK(speak, &Speech::speak, TJS_STATICMEMBER);
	NCB_METHOD(cancel);
}

static void speechUnregist() { backend.reset(); }
NCB_PRE_UNREGIST_CALLBACK(speechUnregist);
