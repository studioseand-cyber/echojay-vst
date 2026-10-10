#pragma once
// ejwav.h (limiter_ab_guard, 7 Oct 2026): the smallest WAV reader/writer that reads what a DAW bounces.
// Reads RIFF/WAVE with fmt tags 1 (PCM 16/24/32), 3 (float 32/64) and 0xFFFE (extensible, either sub-format);
// writes 32-bit float. Deinterleaved doubles out, because every metric downstream is computed in double.
// No JUCE: this harness must build without the plugin library (the rule for session L is "no full plugin builds").
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>

namespace ejwav {

struct Audio
{
    double sampleRate = 0.0;
    std::vector<std::vector<double>> ch;   // ch[c][n]
    int    channels()  const { return (int) ch.size(); }
    size_t frames()    const { return ch.empty() ? 0 : ch[0].size(); }
    std::string sourceFormat;               // "float32", "pcm24" ... for the report
};

namespace detail {
inline uint32_t rd32 (const uint8_t* p) { return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24); }
inline uint16_t rd16 (const uint8_t* p) { return (uint16_t) ((uint32_t) p[0] | ((uint32_t) p[1] << 8)); }
inline void wr32 (std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back ((uint8_t) ((x >> (8 * i)) & 0xFF)); }
inline void wr16 (std::vector<uint8_t>& v, uint16_t x) { v.push_back ((uint8_t) (x & 0xFF)); v.push_back ((uint8_t) ((x >> 8) & 0xFF)); }
}

inline Audio read (const std::string& path)
{
    std::FILE* f = std::fopen (path.c_str(), "rb");
    if (! f) throw std::runtime_error ("cannot open " + path);
    std::vector<uint8_t> bytes;
    { uint8_t buf[1 << 16]; size_t n; while ((n = std::fread (buf, 1, sizeof buf, f)) > 0) bytes.insert (bytes.end(), buf, buf + n); }
    std::fclose (f);
    if (bytes.size() < 12 || std::memcmp (bytes.data(), "RIFF", 4) != 0 || std::memcmp (bytes.data() + 8, "WAVE", 4) != 0)
        throw std::runtime_error (path + ": not a RIFF/WAVE file");

    uint16_t fmtTag = 0, channels = 0, bits = 0; uint32_t rate = 0; bool haveFmt = false;
    size_t dataPos = 0, dataLen = 0;
    size_t pos = 12;
    while (pos + 8 <= bytes.size())
    {
        const uint32_t len = detail::rd32 (bytes.data() + pos + 4);
        const uint8_t* body = bytes.data() + pos + 8;
        const size_t avail = bytes.size() - (pos + 8);
        if (std::memcmp (bytes.data() + pos, "fmt ", 4) == 0 && len >= 16 && len <= avail)
        {
            fmtTag = detail::rd16 (body); channels = detail::rd16 (body + 2); rate = detail::rd32 (body + 4); bits = detail::rd16 (body + 14);
            if (fmtTag == 0xFFFE && len >= 40) fmtTag = detail::rd16 (body + 24);   // extensible: the sub-format GUID's first two bytes
            haveFmt = true;
        }
        else if (std::memcmp (bytes.data() + pos, "data", 4) == 0)
        {
            dataPos = pos + 8; dataLen = len > avail ? avail : len;   // a still-open or truncated file: take what is there
            break;
        }
        pos += 8 + (size_t) len + (len & 1);
    }
    if (! haveFmt || dataPos == 0) throw std::runtime_error (path + ": no fmt/data chunk");
    if (channels == 0 || rate == 0) throw std::runtime_error (path + ": fmt chunk is empty");

    Audio a; a.sampleRate = (double) rate; a.ch.resize (channels);
    const size_t bytesPerSample = bits / 8;
    if (bytesPerSample == 0) throw std::runtime_error (path + ": zero bits per sample");
    const size_t frames = dataLen / (bytesPerSample * channels);
    for (auto& c : a.ch) c.resize (frames);
    const uint8_t* p = bytes.data() + dataPos;
    if (fmtTag == 3 && bits == 32) { a.sourceFormat = "float32"; for (size_t n = 0; n < frames; ++n) for (int c = 0; c < channels; ++c) { float v; std::memcpy (&v, p, 4); p += 4; a.ch[(size_t) c][n] = v; } }
    else if (fmtTag == 3 && bits == 64) { a.sourceFormat = "float64"; for (size_t n = 0; n < frames; ++n) for (int c = 0; c < channels; ++c) { double v; std::memcpy (&v, p, 8); p += 8; a.ch[(size_t) c][n] = v; } }
    else if (fmtTag == 1 && bits == 16) { a.sourceFormat = "pcm16"; for (size_t n = 0; n < frames; ++n) for (int c = 0; c < channels; ++c) { const int16_t v = (int16_t) detail::rd16 (p); p += 2; a.ch[(size_t) c][n] = v / 32768.0; } }
    else if (fmtTag == 1 && bits == 24) { a.sourceFormat = "pcm24"; for (size_t n = 0; n < frames; ++n) for (int c = 0; c < channels; ++c) { int32_t v = (int32_t) (((uint32_t) p[0] << 8) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 24)); v >>= 8; p += 3; a.ch[(size_t) c][n] = v / 8388608.0; } }
    else if (fmtTag == 1 && bits == 32) { a.sourceFormat = "pcm32"; for (size_t n = 0; n < frames; ++n) for (int c = 0; c < channels; ++c) { const int32_t v = (int32_t) detail::rd32 (p); p += 4; a.ch[(size_t) c][n] = v / 2147483648.0; } }
    else throw std::runtime_error (path + ": unsupported format tag " + std::to_string (fmtTag) + " / " + std::to_string (bits) + " bits");
    return a;
}

inline void writeFloat32 (const std::string& path, const Audio& a)
{
    const int channels = a.channels(); const size_t frames = a.frames();
    std::vector<uint8_t> v; v.reserve (44 + frames * (size_t) channels * 4);
    const uint32_t dataLen = (uint32_t) (frames * (size_t) channels * 4);
    v.insert (v.end(), { 'R','I','F','F' }); detail::wr32 (v, 36 + dataLen); v.insert (v.end(), { 'W','A','V','E' });
    v.insert (v.end(), { 'f','m','t',' ' }); detail::wr32 (v, 16); detail::wr16 (v, 3); detail::wr16 (v, (uint16_t) channels);
    detail::wr32 (v, (uint32_t) a.sampleRate); detail::wr32 (v, (uint32_t) a.sampleRate * (uint32_t) channels * 4); detail::wr16 (v, (uint16_t) (channels * 4)); detail::wr16 (v, 32);
    v.insert (v.end(), { 'd','a','t','a' }); detail::wr32 (v, dataLen);
    for (size_t n = 0; n < frames; ++n) for (int c = 0; c < channels; ++c) { const float x = (float) a.ch[(size_t) c][n]; uint8_t b[4]; std::memcpy (b, &x, 4); v.insert (v.end(), b, b + 4); }
    std::FILE* f = std::fopen (path.c_str(), "wb");
    if (! f) throw std::runtime_error ("cannot write " + path);
    std::fwrite (v.data(), 1, v.size(), f); std::fclose (f);
}

} // namespace ejwav
