#ifndef SMS_MP_WIRE_H
#define SMS_MP_WIRE_H

// Little-endian byte writing and reading for the multiplayer packets, the same
// on every host. A reader that runs past its end turns `ok` false and reads
// zeros from then on, so a short or forged packet is dropped, never trusted.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mp {

struct Writer {
	uint8_t* p;
	size_t cap, len;
	bool ok;
	Writer(void* buf, size_t size) : p((uint8_t*)buf), cap(size), len(0), ok(true) {}
	void bytes(const void* d, size_t n)
	{
		if (!ok || len + n > cap) {
			ok = false;
			return;
		}
		memcpy(p + len, d, n);
		len += n;
	}
	void u8(uint8_t v) { bytes(&v, 1); }
	void u16(uint16_t v)
	{
		uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
		bytes(b, 2);
	}
	void u32(uint32_t v)
	{
		uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
		bytes(b, 4);
	}
	void u64(uint64_t v)
	{
		u32((uint32_t)v);
		u32((uint32_t)(v >> 32));
	}
	void s16(int16_t v) { u16((uint16_t)v); }
	void s32(int32_t v) { u32((uint32_t)v); }
	void f32(float v)
	{
		uint32_t u;
		memcpy(&u, &v, 4);
		u32(u);
	}
	// a length-prefixed string, cut to `max` bytes
	void str(const char* s, size_t max)
	{
		size_t n = s ? strlen(s) : 0;
		if (n > max)
			n = max;
		u8((uint8_t)n);
		bytes(s, n);
	}
};

struct Reader {
	const uint8_t* p;
	size_t len, pos;
	bool ok;
	Reader(const void* buf, size_t size) : p((const uint8_t*)buf), len(size), pos(0), ok(true) {}
	bool bytes(void* d, size_t n)
	{
		if (!ok || pos + n > len) {
			ok = false;
			memset(d, 0, n);
			return false;
		}
		memcpy(d, p + pos, n);
		pos += n;
		return true;
	}
	const uint8_t* skip(size_t n)
	{
		if (!ok || pos + n > len) {
			ok = false;
			return nullptr;
		}
		const uint8_t* r = p + pos;
		pos += n;
		return r;
	}
	uint8_t u8()
	{
		uint8_t v;
		bytes(&v, 1);
		return v;
	}
	uint16_t u16()
	{
		uint8_t b[2];
		bytes(b, 2);
		return (uint16_t)(b[0] | b[1] << 8);
	}
	uint32_t u32()
	{
		uint8_t b[4];
		bytes(b, 4);
		return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
	}
	uint64_t u64()
	{
		const uint64_t lo = u32();
		return lo | (uint64_t)u32() << 32;
	}
	int16_t s16() { return (int16_t)u16(); }
	int32_t s32() { return (int32_t)u32(); }
	float f32()
	{
		uint32_t u = u32();
		float v;
		memcpy(&v, &u, 4);
		return v;
	}
	// into a buffer of `size` bytes, always terminated
	void str(char* out, size_t size)
	{
		size_t n = u8();
		const uint8_t* s = skip(n);
		size_t c = s ? (n < size - 1 ? n : size - 1) : 0;
		if (c)
			memcpy(out, s, c);
		out[c] = 0;
	}
	size_t left() const { return ok ? len - pos : 0; }
};

} // namespace mp

#endif
