// speech_windows.cpp - Windows: SAPI 5, spoken into a WAV file on a thread of its own
//
// SPDX-License-Identifier: 0BSD
#include "backend.h"

#if defined(_WIN32)

#include <windows.h>
#include <sapi.h>

#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>

namespace speech {

namespace {

// SPDFID_WaveFormatEx, written out so nothing has to link sapi.lib
const GUID kWaveFormatEx = { 0xC31ADBAE, 0x527F, 0x4ff5, { 0xA2, 0x30, 0xF6, 0x2B, 0xB6, 0x1F, 0xF7, 0x0C } };
const wchar_t *const kVoicesCategory = L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech\\Voices";

std::wstring wide(const std::string &s) {
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
	std::wstring w(size_t(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
	return w;
}

std::string narrow(const wchar_t *w) {
	if (!w || !*w) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
	if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
	return s;
}

template <typename T>
void release(T *&p) {
	if (p) p->Release();
	p = nullptr;
}

// A voice token's language: its Attributes\Language, a hex LCID ("409", or "411;409")
std::string tokenLanguage(ISpObjectToken *token) {
	ISpDataKey *attrs = nullptr;
	std::string out;
	if (SUCCEEDED(token->OpenKey(L"Attributes", &attrs)) && attrs) {
		WCHAR *lang = nullptr;
		if (SUCCEEDED(attrs->GetStringValue(L"Language", &lang)) && lang) {
			LCID lcid = LCID(wcstoul(lang, nullptr, 16));
			wchar_t name[LOCALE_NAME_MAX_LENGTH] = { 0 };
			if (lcid && LCIDToLocaleName(lcid, name, LOCALE_NAME_MAX_LENGTH, 0) > 0) out = narrow(name);
			CoTaskMemFree(lang);
		}
		attrs->Release();
	}
	return out;
}

std::vector<Voice> listVoices() {
	std::vector<Voice> out;
	ISpObjectTokenCategory *cat = nullptr;
	IEnumSpObjectTokens *tokens = nullptr;
	if (FAILED(CoCreateInstance(__uuidof(SpObjectTokenCategory), nullptr, CLSCTX_ALL, __uuidof(ISpObjectTokenCategory),
	                            reinterpret_cast<void **>(&cat))) ||
	    FAILED(cat->SetId(kVoicesCategory, FALSE)) || FAILED(cat->EnumTokens(nullptr, nullptr, &tokens))) {
		release(cat);
		return out;
	}
	ISpObjectToken *token = nullptr;
	while (tokens->Next(1, &token, nullptr) == S_OK && token) {
		Voice v;
		WCHAR *id = nullptr, *name = nullptr;
		if (SUCCEEDED(token->GetId(&id)) && id) {
			v.id = narrow(id);
			CoTaskMemFree(id);
		}
		if (SUCCEEDED(token->GetStringValue(nullptr, &name)) && name) {
			v.name = narrow(name);
			CoTaskMemFree(name);
		}
		v.language = tokenLanguage(token);
		if (!v.id.empty()) out.push_back(v);
		release(token);
	}
	release(tokens);
	release(cat);
	return out;
}

// SAPI's rate is -10..10, 10 about three times as fast; its pitch -10..10, 10 an octave up
long sapiRate(double percent) { return long(std::lround(clampd(10.0 * std::log(clampd(percent, 10, 1000) / 100.0) / std::log(3.0), -10, 10))); }
long sapiPitch(double percent) { return long(std::lround(clampd(10.0 * std::log2(clampd(percent, 10, 1000) / 100.0), -10, 10))); }

class SapiBackend : public Backend {
	std::thread worker;
	std::atomic<int> state{ int(State::Idle) };
	std::atomic<bool> stop{ false };
	std::mutex lock;
	std::string error;
	bool checked = false, have = false;
	std::vector<Voice> cached;

	void join() {
		stop = true;
		if (worker.joinable()) worker.join();
		stop = false;
	}

	void setError(const std::string &e) {
		std::lock_guard<std::mutex> hold(lock);
		error = e;
	}

	void run(Request r) {
		HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		ISpVoice *voice = nullptr;
		ISpStream *stream = nullptr;
		ISpObjectToken *token = nullptr;
		bool ok = false;
		do {
			if (FAILED(CoCreateInstance(__uuidof(SpVoice), nullptr, CLSCTX_ALL, __uuidof(ISpVoice),
			                            reinterpret_cast<void **>(&voice)))) {
				setError("no SAPI voice");
				break;
			}
			if (FAILED(CoCreateInstance(__uuidof(SpStream), nullptr, CLSCTX_ALL, __uuidof(ISpStream),
			                            reinterpret_cast<void **>(&stream)))) {
				setError("no SAPI stream");
				break;
			}
			WAVEFORMATEX fmt;
			ZeroMemory(&fmt, sizeof fmt);
			fmt.wFormatTag = WAVE_FORMAT_PCM;
			fmt.nChannels = 1;
			fmt.nSamplesPerSec = 22050;
			fmt.wBitsPerSample = 16;
			fmt.nBlockAlign = 2;
			fmt.nAvgBytesPerSec = 44100;
			std::wstring file = wide(r.file);
			if (FAILED(stream->BindToFile(file.c_str(), SPFM_CREATE_ALWAYS, &kWaveFormatEx, &fmt, 0))) {
				setError("can't write " + r.file);
				break;
			}
			if (!r.voice.empty() &&
			    SUCCEEDED(CoCreateInstance(__uuidof(SpObjectToken), nullptr, CLSCTX_ALL, __uuidof(ISpObjectToken),
			                               reinterpret_cast<void **>(&token)))) {
				std::wstring id = wide(r.voice);
				if (SUCCEEDED(token->SetId(nullptr, id.c_str(), FALSE))) voice->SetVoice(token);
			}
			voice->SetOutput(stream, TRUE);
			voice->SetRate(sapiRate(r.rate));
			voice->SetVolume(USHORT(clampd(r.volume, 0, 100)));
			std::string xml = "<pitch absmiddle=\"" + std::to_string(sapiPitch(r.pitch)) + "\">" + xmlEscape(r.text) + "</pitch>";
			std::wstring text = wide(xml);
			if (FAILED(voice->Speak(text.c_str(), SPF_ASYNC | SPF_IS_XML, nullptr))) {
				setError("SAPI couldn't speak the line");
				break;
			}
			// wait for it, or a cancel
			for (;;) {
				if (stop) {
					voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
					break;
				}
				if (voice->WaitUntilDone(50) == S_OK) {
					ok = true;
					break;
				}
			}
		} while (false);
		if (stream) stream->Close();
		release(token);
		release(stream);
		release(voice);
		if (SUCCEEDED(init)) CoUninitialize();
		if (!stop) state = int(ok ? State::Done : State::Failed);
	}

public:
	~SapiBackend() override { join(); }

	bool available() override {
		if (!checked) {
			checked = true;
			have = !voices().empty();
		}
		return have;
	}

	std::vector<Voice> voices() override {
		if (!cached.empty()) return cached;
		// on a thread of its own: the engine's thread may be in another COM apartment
		std::vector<Voice> found;
		std::thread t([&found] {
			HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			found = listVoices();
			if (SUCCEEDED(init)) CoUninitialize();
		});
		t.join();
		cached = found;
		return cached;
	}

	bool start(const Request &r, std::string &err) override {
		cancel();
		if (r.file.empty()) {
			err = "no file to speak into";
			return false;
		}
		setError("");
		state = int(State::Busy);
		worker = std::thread(&SapiBackend::run, this, r);
		return true;
	}

	State poll(std::string &err) override {
		{
			std::lock_guard<std::mutex> hold(lock);
			err = error;
		}
		State s = State(state.load());
		if (s != State::Busy && worker.joinable()) worker.join();
		return s;
	}

	void cancel() override {
		join();
		state = int(State::Idle);
	}
};

} // namespace

Backend *makeBackend() { return new SapiBackend(); }

} // namespace speech

#endif
