// speech_android.cpp - Android: android.speech.tts.TextToSpeech's synthesizeToFile, through JNI
// on the engine's thread (SDL's, which SDL has attached to the VM)
//
// The engine is made without a listener (TextToSpeech tells a null one nothing), so a request
// is tried again until the engine is ready, and a line is done when the engine no longer
// speaks (isSpeaking covers writing to a file) and the file has stopped growing.
//
// On Android 11 and later the app's manifest must ask to see speech engines:
//   <queries><intent><action android:name="android.intent.action.TTS_SERVICE" /></intent></queries>
//
// SPDX-License-Identifier: 0BSD
#include "backend.h"

#if defined(__ANDROID__)

#include <cstdarg>
#include <jni.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
void *SDL_GetAndroidJNIEnv(void);
void *SDL_GetAndroidActivity(void);
}

namespace speech {

namespace {

bool failed(JNIEnv *env) {
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
		return true;
	}
	return false;
}

std::string str(JNIEnv *env, jstring s) {
	if (!s) return std::string();
	const char *c = env->GetStringUTFChars(s, nullptr);
	std::string out = c ? c : "";
	if (c) env->ReleaseStringUTFChars(s, c);
	return out;
}

long fileSize(const std::string &path) {
	struct stat st;
	return stat(path.c_str(), &st) == 0 ? long(st.st_size) : -1;
}

class AndroidBackend : public Backend {
	jobject tts = nullptr; // a global reference
	bool tried = false;
	State state = State::Idle;
	std::string error;
	Request request;
	bool submitted = false, sawSpeaking = false;
	int tries = 0, polls = 0, stable = 0;
	long lastSize = -1;

	JNIEnv *env() { return static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv()); }

	bool ensure() {
		if (tts) return true;
		if (tried) return false;
		tried = true;
		JNIEnv *e = env();
		if (!e) return false;
		e->PushLocalFrame(16);
		jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
		jclass cls = e->FindClass("android/speech/tts/TextToSpeech");
		if (!failed(e) && cls && activity) {
			jmethodID ctor = e->GetMethodID(cls, "<init>",
			                                "(Landroid/content/Context;Landroid/speech/tts/TextToSpeech$OnInitListener;)V");
			if (!failed(e) && ctor) {
				jobject obj = e->NewObject(cls, ctor, activity, nullptr);
				if (!failed(e) && obj) tts = e->NewGlobalRef(obj);
			}
		}
		e->PopLocalFrame(nullptr);
		return tts != nullptr;
	}

	// Calls a TextToSpeech method returning int, -1 on an exception
	int callInt(JNIEnv *e, const char *name, const char *sig, ...) {
		jclass cls = e->GetObjectClass(tts);
		jmethodID m = e->GetMethodID(cls, name, sig);
		if (failed(e) || !m) return -1;
		va_list args;
		va_start(args, sig);
		int r = e->CallIntMethodV(tts, m, args);
		va_end(args);
		return failed(e) ? -1 : r;
	}

	// Calls a TextToSpeech method returning boolean, false on an exception (CheckJNI, on in a
	// debuggable app, aborts the process when the call's return type doesn't match the method's)
	bool callBool(JNIEnv *e, const char *name, const char *sig) {
		jclass cls = e->GetObjectClass(tts);
		jmethodID m = e->GetMethodID(cls, name, sig);
		if (failed(e) || !m) return false;
		jboolean r = e->CallBooleanMethod(tts, m);
		return failed(e) ? false : r == JNI_TRUE;
	}

	// The Voice object whose name is `name`, or null
	jobject findVoice(JNIEnv *e, const std::string &name) {
		jclass cls = e->GetObjectClass(tts);
		jmethodID getVoices = e->GetMethodID(cls, "getVoices", "()Ljava/util/Set;");
		if (failed(e) || !getVoices) return nullptr;
		jobject set = e->CallObjectMethod(tts, getVoices);
		if (failed(e) || !set) return nullptr;
		jclass setCls = e->FindClass("java/util/Set");
		jmethodID toArray = e->GetMethodID(setCls, "toArray", "()[Ljava/lang/Object;");
		jobjectArray arr = static_cast<jobjectArray>(e->CallObjectMethod(set, toArray));
		if (failed(e) || !arr) return nullptr;
		jclass voiceCls = e->FindClass("android/speech/tts/Voice");
		jmethodID getName = e->GetMethodID(voiceCls, "getName", "()Ljava/lang/String;");
		jsize n = e->GetArrayLength(arr);
		for (jsize k = 0; k < n; k++) {
			jobject v = e->GetObjectArrayElement(arr, k);
			if (str(e, static_cast<jstring>(e->CallObjectMethod(v, getName))) == name) return v;
			failed(e);
		}
		return nullptr;
	}

	// Hands the request to the engine; false while the engine isn't ready
	bool submit() {
		JNIEnv *e = env();
		if (!e || !tts) return false;
		e->PushLocalFrame(64);
		jclass cls = e->GetObjectClass(tts);
		jmethodID setRate = e->GetMethodID(cls, "setSpeechRate", "(F)I");
		jmethodID setPitch = e->GetMethodID(cls, "setPitch", "(F)I");
		if (!failed(e) && setRate && setPitch) {
			e->CallIntMethod(tts, setRate, jfloat(clampd(request.rate / 100.0, 0.1, 4)));
			failed(e);
			e->CallIntMethod(tts, setPitch, jfloat(clampd(request.pitch / 100.0, 0.25, 4)));
			failed(e);
		}
		if (!request.voice.empty()) {
			if (jobject v = findVoice(e, request.voice)) {
				jmethodID setVoice = e->GetMethodID(cls, "setVoice", "(Landroid/speech/tts/Voice;)I");
				if (!failed(e) && setVoice) {
					e->CallIntMethod(tts, setVoice, v);
					failed(e);
				}
			}
		}
		jclass bundleCls = e->FindClass("android/os/Bundle");
		jobject bundle = e->NewObject(bundleCls, e->GetMethodID(bundleCls, "<init>", "()V"));
		jmethodID putFloat = e->GetMethodID(bundleCls, "putFloat", "(Ljava/lang/String;F)V");
		e->CallVoidMethod(bundle, putFloat, e->NewStringUTF("volume"), jfloat(clampd(request.volume / 100.0, 0, 1)));
		failed(e);
		jclass fileCls = e->FindClass("java/io/File");
		jobject file = e->NewObject(fileCls, e->GetMethodID(fileCls, "<init>", "(Ljava/lang/String;)V"),
		                            e->NewStringUTF(request.file.c_str()));
		jstring text = e->NewStringUTF(request.text.c_str());
		jmethodID synth = e->GetMethodID(cls, "synthesizeToFile",
		                                 "(Ljava/lang/CharSequence;Landroid/os/Bundle;Ljava/io/File;Ljava/lang/String;)I");
		int r = -1;
		if (!failed(e) && synth && file && text) {
			r = e->CallIntMethod(tts, synth, text, bundle, file, e->NewStringUTF("kagex"));
			if (failed(e)) r = -1;
		}
		e->PopLocalFrame(nullptr);
		return r == 0;
	}

public:
	~AndroidBackend() override {
		cancel();
		if (tts) {
			JNIEnv *e = env();
			if (e) {
				jclass cls = e->GetObjectClass(tts);
				jmethodID shutdown = e->GetMethodID(cls, "shutdown", "()V");
				if (!failed(e) && shutdown) {
					e->CallVoidMethod(tts, shutdown);
					failed(e);
				}
				e->DeleteGlobalRef(tts);
			}
			tts = nullptr;
		}
	}

	bool available() override {
		if (!ensure()) return false;
		JNIEnv *e = env();
		e->PushLocalFrame(16);
		bool any = false;
		jclass cls = e->GetObjectClass(tts);
		jmethodID getEngines = e->GetMethodID(cls, "getEngines", "()Ljava/util/List;");
		if (!failed(e) && getEngines) {
			jobject list = e->CallObjectMethod(tts, getEngines);
			if (!failed(e) && list) {
				jclass listCls = e->FindClass("java/util/List");
				jint n = e->CallIntMethod(list, e->GetMethodID(listCls, "size", "()I"));
				any = !failed(e) && n > 0;
			}
		}
		e->PopLocalFrame(nullptr);
		return any;
	}

	std::vector<Voice> voices() override {
		std::vector<Voice> out;
		if (!ensure()) return out;
		JNIEnv *e = env();
		e->PushLocalFrame(256);
		jclass cls = e->GetObjectClass(tts);
		jmethodID getVoices = e->GetMethodID(cls, "getVoices", "()Ljava/util/Set;");
		jobject set = (!failed(e) && getVoices) ? e->CallObjectMethod(tts, getVoices) : nullptr;
		if (!failed(e) && set) {
			jclass setCls = e->FindClass("java/util/Set");
			jobjectArray arr =
			    static_cast<jobjectArray>(e->CallObjectMethod(set, e->GetMethodID(setCls, "toArray", "()[Ljava/lang/Object;")));
			jclass voiceCls = e->FindClass("android/speech/tts/Voice");
			jmethodID getName = e->GetMethodID(voiceCls, "getName", "()Ljava/lang/String;");
			jmethodID getLocale = e->GetMethodID(voiceCls, "getLocale", "()Ljava/util/Locale;");
			jclass localeCls = e->FindClass("java/util/Locale");
			jmethodID tag = e->GetMethodID(localeCls, "toLanguageTag", "()Ljava/lang/String;");
			if (!failed(e) && arr) {
				jsize n = e->GetArrayLength(arr);
				for (jsize k = 0; k < n && k < 400; k++) {
					jobject v = e->GetObjectArrayElement(arr, k);
					Voice voice;
					voice.id = str(e, static_cast<jstring>(e->CallObjectMethod(v, getName)));
					voice.name = voice.id;
					jobject loc = e->CallObjectMethod(v, getLocale);
					if (!failed(e) && loc) voice.language = str(e, static_cast<jstring>(e->CallObjectMethod(loc, tag)));
					failed(e);
					if (!voice.id.empty()) out.push_back(voice);
					e->DeleteLocalRef(v);
				}
			}
		}
		failed(e);
		e->PopLocalFrame(nullptr);
		return out;
	}

	bool start(const Request &r, std::string &err) override {
		cancel();
		if (!ensure()) {
			err = "no TextToSpeech";
			return false;
		}
		if (r.file.empty()) {
			err = "no file to speak into";
			return false;
		}
		request = r;
		unlink(r.file.c_str());
		submitted = submit();
		sawSpeaking = false;
		tries = 1;
		polls = 0;
		stable = 0;
		lastSize = -1;
		error.clear();
		state = State::Busy;
		return true;
	}

	State poll(std::string &err) override {
		if (state == State::Busy) {
			JNIEnv *e = env();
			if (!submitted) {
				// the engine binds in the background: try again, for about 8 s of polls
				submitted = submit();
				if (!submitted && ++tries > 160) {
					state = State::Failed;
					error = "the speech engine didn't get ready";
				}
			} else if (e) {
				polls++;
				bool speaking = callBool(e, "isSpeaking", "()Z");
				if (speaking) sawSpeaking = true;
				long size = fileSize(request.file);
				stable = (size > 44 && size == lastSize) ? stable + 1 : 0;
				lastSize = size;
				if (!speaking && size > 44 && (sawSpeaking || stable >= 3)) state = State::Done;
				else if (polls > 1200) {
					state = State::Failed;
					error = "the speech engine wrote nothing";
				}
			}
		}
		err = error;
		return state;
	}

	void cancel() override {
		if (tts && state == State::Busy) {
			JNIEnv *e = env();
			if (e) callInt(e, "stop", "()I");
		}
		state = State::Idle;
	}
};

} // namespace

Backend *makeBackend() { return new AndroidBackend(); }

} // namespace speech

#endif
