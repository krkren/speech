// What needs no engine: the WAV writer, XML escaping, and on Linux the eSpeak backend's handling
// of a stand-in espeak-ng (a script on PATH that writes what it was asked into the WAV file)
//
// SPDX-License-Identifier: 0BSD
#include "backend.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(__linux__)
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace speech;

static int failures = 0;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
			failures++;                                                                                                \
		}                                                                                                              \
	} while (0)

static std::string readAll(const std::string &path) {
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f) return std::string();
	std::string s;
	char buf[4096];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
	std::fclose(f);
	return s;
}

static unsigned le32(const std::string &s, size_t at) {
	return unsigned((unsigned char)s[at]) | unsigned((unsigned char)s[at + 1]) << 8 |
	       unsigned((unsigned char)s[at + 2]) << 16 | unsigned((unsigned char)s[at + 3]) << 24;
}

#if defined(__linux__)
static void writeScript(const std::string &path, const std::string &body) {
	FILE *f = std::fopen(path.c_str(), "w");
	std::fputs(body.c_str(), f);
	std::fclose(f);
	chmod(path.c_str(), 0755);
}

static State wait(Backend &b, std::string &err, int maxMs = 10000) {
	State s = State::Busy;
	for (int t = 0; t < maxMs; t += 20) {
		s = b.poll(err);
		if (s != State::Busy) break;
		usleep(20000);
	}
	return s;
}
#endif

int main() {
	// the WAV writer
	char tmpl[] = "/tmp/speech_testXXXXXX";
	std::string dir;
#if defined(__linux__)
	dir = mkdtemp(tmpl);
#else
	dir = ".";
	(void)tmpl;
#endif
	std::string wav = dir + "/a.wav";
	std::vector<short> samples = { 0, 1000, -1000, 32767, -32768 };
	CHECK(writeWav(wav, samples, 22050, 1));
	std::string w = readAll(wav);
	CHECK(w.size() == 44 + samples.size() * 2);
	CHECK(w.compare(0, 4, "RIFF") == 0 && w.compare(8, 8, "WAVEfmt ") == 0 && w.compare(36, 4, "data") == 0);
	CHECK(le32(w, 4) == w.size() - 8);
	CHECK(le32(w, 24) == 22050);
	CHECK(le32(w, 40) == samples.size() * 2);
	CHECK((unsigned char)w[44 + 6] == 0xff && (unsigned char)w[44 + 7] == 0x7f); // 32767

	CHECK(xmlEscape("a<b & \"c\" 'd'>") == "a&lt;b &amp; &quot;c&quot; &apos;d&apos;&gt;");

#if defined(__linux__)
	// eSpeak, stood in for by a script: what it was asked goes into the "WAV"
	std::string bin = dir + "/bin";
	mkdir(bin.c_str(), 0755);
	writeScript(bin + "/espeak-ng",
	            "#!/bin/sh\n"
	            "if [ \"$1\" = --voices ]; then\n"
	            "  echo 'Pty Language       Age/Gender VoiceName          File                 Other Languages'\n"
	            "  echo ' 5  en-us           --/M      English_(America)  gmw/en-US            (en 3)'\n"
	            "  echo ' 5  ja              --/M      Japanese           jpx/ja'\n"
	            "  exit 0\n"
	            "fi\n"
	            "out=''; text=''\n"
	            "while [ $# -gt 0 ]; do case \"$1\" in -w) out=$2; shift;; -f) text=$2; shift;; esac; shift; done\n"
	            "case \"$(cat \"$text\")\" in *fail*) exit 3;; *slow*) sleep 30;; esac\n"
	            "{ printf 'RIFF....WAVEfmt '; printf '%064d' 0; echo \"$*\"; cat \"$text\"; } > \"$out\"\n");
	std::string path = bin + ":" + (getenv("PATH") ? getenv("PATH") : "");
	setenv("PATH", path.c_str(), 1);

	Backend *b = makeBackend();
	CHECK(b->available());
	CHECK(std::string(b->mode()) == "file");
	std::vector<Voice> vs = b->voices();
	CHECK(vs.size() == 2);
	if (vs.size() == 2) {
		CHECK(vs[0].id == "en-us" && vs[0].name == "English (America)" && vs[0].language == "en-us");
		CHECK(vs[1].id == "ja");
	}

	Request r;
	r.text = "Hello, \xe3\x81\x93\xe3\x82\x93\xe3\x81\xab\xe3\x81\xa1\xe3\x81\xaf.";
	r.voice = "en-us";
	r.rate = 200;
	r.file = dir + "/line.wav";
	std::string err;
	CHECK(b->start(r, err));
	CHECK(wait(*b, err) == State::Done);
	std::string got = readAll(r.file);
	CHECK(got.find(r.text) != std::string::npos);
	CHECK(access((r.file + ".txt").c_str(), F_OK) != 0); // the text's file is gone

	r.text = "please fail";
	CHECK(b->start(r, err));
	CHECK(wait(*b, err) == State::Failed);
	CHECK(!err.empty());

	r.text = "a slow one";
	CHECK(b->start(r, err));
	CHECK(b->poll(err) == State::Busy);
	b->cancel();
	CHECK(b->poll(err) == State::Idle);

	r.file.clear();
	r.text = "no file";
	CHECK(!b->start(r, err));
	delete b;
#endif

	if (failures) {
		std::printf("%d failed\n", failures);
		return 1;
	}
	std::printf("speech_test: all passed\n");
	return 0;
}
