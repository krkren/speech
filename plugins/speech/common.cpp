// common.cpp - what the backends share (backend.h)
//
// SPDX-License-Identifier: 0BSD
#include "backend.h"

#include <cstdio>

namespace speech {

double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

std::string xmlEscape(const std::string &s) {
	std::string out;
	out.reserve(s.size());
	for (char c : s) {
		switch (c) {
		case '&': out += "&amp;"; break;
		case '<': out += "&lt;"; break;
		case '>': out += "&gt;"; break;
		case '"': out += "&quot;"; break;
		case '\'': out += "&apos;"; break;
		default: out += c;
		}
	}
	return out;
}

namespace {
void put16(FILE *f, unsigned v) {
	unsigned char b[2] = { (unsigned char)(v & 0xff), (unsigned char)((v >> 8) & 0xff) };
	fwrite(b, 1, 2, f);
}
void put32(FILE *f, unsigned long v) {
	put16(f, unsigned(v & 0xffff));
	put16(f, unsigned((v >> 16) & 0xffff));
}
} // namespace

bool writeWav(const std::string &path, const std::vector<short> &samples, int rate, int channels) {
	FILE *f = std::fopen(path.c_str(), "wb");
	if (!f) return false;
	unsigned long bytes = (unsigned long)(samples.size() * 2);
	fwrite("RIFF", 1, 4, f);
	put32(f, 36 + bytes);
	fwrite("WAVEfmt ", 1, 8, f);
	put32(f, 16);
	put16(f, 1); // PCM
	put16(f, unsigned(channels));
	put32(f, (unsigned long)rate);
	put32(f, (unsigned long)rate * unsigned(channels) * 2);
	put16(f, unsigned(channels) * 2);
	put16(f, 16);
	fwrite("data", 1, 4, f);
	put32(f, bytes);
	for (short s : samples) put16(f, (unsigned)(unsigned short)s);
	bool ok = !std::ferror(f);
	return std::fclose(f) == 0 && ok;
}

} // namespace speech
