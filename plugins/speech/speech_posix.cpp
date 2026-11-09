// speech_posix.cpp - Linux (and other desktops without a speech API): eSpeak NG, run into a WAV
// file when `espeak-ng` (or `espeak`) is on PATH
//
// SPDX-License-Identifier: 0BSD
#include "backend.h"

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace speech {

namespace {

bool executable(const std::string &path) {
	struct stat st;
	return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(path.c_str(), X_OK) == 0;
}

std::string onPath(const char *program) {
	const char *path = getenv("PATH");
	std::string all = path ? path : "/usr/local/bin:/usr/bin:/bin";
	size_t start = 0;
	while (start <= all.size()) {
		size_t end = all.find(':', start);
		if (end == std::string::npos) end = all.size();
		std::string dir = all.substr(start, end - start);
		if (dir.empty()) dir = ".";
		if (executable(dir + "/" + program)) return dir + "/" + program;
		start = end + 1;
	}
	return std::string();
}

// Runs a program and gives its output (for the voice list); empty when it fails
std::string capture(const std::vector<std::string> &args) {
	std::vector<std::string> owned = args;
	std::vector<char *> argv;
	for (std::string &s : owned) argv.push_back(&s[0]);
	argv.push_back(nullptr);
	int fds[2];
	if (pipe(fds) != 0) return std::string();
	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return std::string();
	}
	if (pid == 0) {
		dup2(fds[1], 1);
		close(fds[0]);
		close(fds[1]);
		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, 0);
			dup2(devnull, 2);
		}
		execv(argv[0], argv.data());
		_exit(127);
	}
	close(fds[1]);
	std::string out;
	char buf[4096];
	for (;;) {
		ssize_t n = read(fds[0], buf, sizeof buf);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) break;
		out.append(buf, size_t(n));
	}
	close(fds[0]);
	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
	}
	return out;
}

class EspeakBackend : public Backend {
	std::string program;
	pid_t pid = -1;
	std::string textFile, outFile;
	State state = State::Idle;
	std::string error;
	std::vector<Voice> cached;
	bool listed = false;

	void reap(bool kill) {
		if (pid <= 0) return;
		if (kill) ::kill(pid, SIGTERM);
		int status = 0;
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
		}
		pid = -1;
	}
	void cleanText() {
		if (!textFile.empty()) unlink(textFile.c_str());
		textFile.clear();
	}

public:
	EspeakBackend() {
		program = onPath("espeak-ng");
		if (program.empty()) program = onPath("espeak");
	}
	~EspeakBackend() override {
		reap(true);
		cleanText();
	}

	bool available() override { return !program.empty(); }

	std::vector<Voice> voices() override {
		if (listed || program.empty()) return cached;
		listed = true;
		// "Pty Language       Age/Gender VoiceName          File                 Other Languages"
		std::istringstream in(capture({ program, "--voices" }));
		std::string line;
		bool header = true;
		while (std::getline(in, line)) {
			if (header) {
				header = false;
				continue;
			}
			std::istringstream cols(line);
			std::string pty, lang, gender, name;
			if (!(cols >> pty >> lang >> gender >> name)) continue;
			Voice v;
			v.id = lang;
			v.name = name;
			for (char &c : v.name)
				if (c == '_') c = ' ';
			v.language = lang;
			cached.push_back(v);
		}
		return cached;
	}

	bool start(const Request &r, std::string &err) override {
		cancel();
		if (program.empty()) {
			err = "no espeak-ng or espeak on PATH";
			return false;
		}
		if (r.file.empty()) {
			err = "no file to speak into";
			return false;
		}
		// the text through a file: no limit on its length, and it stays UTF-8 (-b 1)
		textFile = r.file + ".txt";
		FILE *f = fopen(textFile.c_str(), "wb");
		if (!f) {
			err = "can't write " + textFile;
			textFile.clear();
			return false;
		}
		fwrite(r.text.data(), 1, r.text.size(), f);
		fclose(f);
		outFile = r.file;
		unlink(outFile.c_str());
		// eSpeak's units: 175 words a minute, pitch 50 of 0-99, amplitude 100 of 0-200
		std::vector<std::string> args = { program, "-b", "1", "-w", outFile, "-f", textFile };
		args.push_back("-s");
		args.push_back(std::to_string(int(clampd(175 * r.rate / 100.0, 80, 500))));
		args.push_back("-p");
		args.push_back(std::to_string(int(clampd(50 * r.pitch / 100.0, 0, 99))));
		args.push_back("-a");
		args.push_back(std::to_string(int(clampd(100 * r.volume / 100.0, 0, 200))));
		if (!r.voice.empty()) {
			args.push_back("-v");
			args.push_back(r.voice);
		}
		std::vector<char *> argv;
		for (std::string &s : args) argv.push_back(&s[0]);
		argv.push_back(nullptr);
		pid = fork();
		if (pid < 0) {
			err = std::string("fork: ") + strerror(errno);
			cleanText();
			return false;
		}
		if (pid == 0) {
			int devnull = open("/dev/null", O_RDWR);
			if (devnull >= 0) {
				dup2(devnull, 0);
				dup2(devnull, 1);
				dup2(devnull, 2);
			}
			execv(argv[0], argv.data());
			_exit(127);
		}
		state = State::Busy;
		error.clear();
		return true;
	}

	State poll(std::string &err) override {
		if (state == State::Busy && pid > 0) {
			int status = 0;
			pid_t r = waitpid(pid, &status, WNOHANG);
			if (r == pid) {
				pid = -1;
				cleanText();
				struct stat st;
				if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && stat(outFile.c_str(), &st) == 0 && st.st_size > 44) {
					state = State::Done;
				} else {
					state = State::Failed;
					error = "espeak failed (status " + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) + ")";
				}
			}
		}
		err = error;
		return state;
	}

	void cancel() override {
		reap(true);
		cleanText();
		state = State::Idle;
	}
};

} // namespace

Backend *makeBackend() { return new EspeakBackend(); }

} // namespace speech

#endif
