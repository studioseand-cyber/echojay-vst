// AudioToolbox MUST come before JuceHeader: JUCE's `using namespace juce`
// makes juce::Point/juce::AudioBuffer ambiguous with the CoreAudio globals
// if the SDK headers are parsed second.
#if defined(__APPLE__)
 #include <AudioToolbox/AudioToolbox.h>
#endif

#include "CodecRender.h"
#include "MeterEngine.h"
#include "NativeClip.h"   // EchoJay_NSLog

namespace CodecRender
{

const std::vector<Preset>& presets()
{
    static const std::vector<Preset> list = []
    {
        std::vector<Preset> p;
        // Names exclude "kbps": the card's second line prints the bitrate,
        // and slot/chip labels read "AAC 256 · normalised"
#if JUCE_MAC
        p.push_back({ "aac256", "AAC 256", "Apple Music-style",       256, true });
        p.push_back({ "aac128", "AAC 128", "AAC, lower tier",         128, true });
#endif
        p.push_back({ "ogg320", "Ogg Vorbis 320", "Spotify-style, high",   320, false });
        p.push_back({ "ogg160", "Ogg Vorbis 160", "Spotify-style, normal", 160, false });
        return p;
    }();
    return list;
}

static juce::File cacheDir()
{
    auto d = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                 .getChildFile("EchoJay").getChildFile("codec_cache");
    d.createDirectory();
    return d;
}

static void evictCache(int keepFiles = 32)   // 16 renders = 32 files (orig+rend)
{
    juce::Array<juce::File> files = cacheDir().findChildFiles(juce::File::findFiles, false, "*.wav");
    if (files.size() <= keepFiles) return;
    std::sort(files.begin(), files.end(),
              [](const juce::File& a, const juce::File& b)
              { return a.getLastModificationTime() < b.getLastModificationTime(); });
    for (int i = 0; i < files.size() - keepFiles; ++i)
        files.getReference(i).deleteFile();
}

static bool readWholeFile(const juce::String& path, juce::AudioBuffer<float>& out, double& sr)
{
    juce::AudioFormatManager fmt;
    fmt.registerBasicFormats();   // WAV/AIFF/Ogg/FLAC (+ CoreAudio codecs on mac)
    std::unique_ptr<juce::AudioFormatReader> r(fmt.createReaderFor(juce::File(path)));
    if (r == nullptr || r->lengthInSamples <= 0) return false;
    sr = r->sampleRate;
    out.setSize(juce::jmin(2, (int) r->numChannels) == 1 ? 2 : 2, (int) r->lengthInSamples);
    juce::AudioBuffer<float> tmp((int) r->numChannels, (int) r->lengthInSamples);
    r->read(&tmp, 0, (int) r->lengthInSamples, 0, true, true);
    // up-mix mono -> stereo so every downstream stage is uniform
    out.copyFrom(0, 0, tmp, 0, 0, tmp.getNumSamples());
    out.copyFrom(1, 0, tmp, tmp.getNumChannels() >= 2 ? 1 : 0, 0, tmp.getNumSamples());
    return true;
}

static bool writeWav(const juce::File& f, const juce::AudioBuffer<float>& buf, double sr)
{
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> os(f.createOutputStream());
    if (os == nullptr) return false;
    std::unique_ptr<juce::AudioFormatWriter> w(
        wav.createWriterFor(os.get(), sr, 2, 32, {}, 0));
    if (w == nullptr) return false;
    os.release();   // writer owns the stream now
    return w->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
}

// Integrated LUFS via the SAME engine the meters use (K-weighting identical)
static float measureIntegratedLufs(const juce::AudioBuffer<float>& buf, double sr)
{
    MeterEngine me;
    me.prepare(sr, 4096);
    const int n = buf.getNumSamples();
    for (int pos = 0; pos < n; pos += 4096)
    {
        const int len = juce::jmin(4096, n - pos);
        me.processBlock(buf.getReadPointer(0) + pos, buf.getReadPointer(1) + pos, len);
    }
    return me.getMeterData().integrated;
}

static std::vector<float> makeThumb(const juce::AudioBuffer<float>& buf, int points = 300)
{
    std::vector<float> t((size_t) points, 0.0f);
    const int n = buf.getNumSamples();
    if (n <= 0) return t;
    for (int i = 0; i < points; ++i)
    {
        const int a = (int) ((juce::int64) n * i / points);
        const int b = juce::jmax(a + 1, (int) ((juce::int64) n * (i + 1) / points));
        float mx = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* s = buf.getReadPointer(ch);
            for (int j = a; j < b && j < n; ++j) mx = juce::jmax(mx, std::abs(s[j]));
        }
        t[(size_t) i] = mx;
    }
    return t;
}

static bool encodeOgg(const juce::AudioBuffer<float>& buf, double sr, int kbps, const juce::File& out)
{
    juce::OggVorbisAudioFormat ogg;
    // Pick the quality option whose label contains the target kbps; JUCE's
    // options are labelled "64 kbps".."500 kbps". Fallback: highest.
    auto opts = ogg.getQualityOptions();
    int qIdx = opts.size() - 1;
    for (int i = 0; i < opts.size(); ++i)
        if (opts[i].contains(juce::String(kbps))) { qIdx = i; break; }
    out.deleteFile();
    std::unique_ptr<juce::FileOutputStream> os(out.createOutputStream());
    if (os == nullptr) return false;
    std::unique_ptr<juce::AudioFormatWriter> w(
        ogg.createWriterFor(os.get(), sr, 2, 16, {}, qIdx));
    if (w == nullptr) return false;
    os.release();
    return w->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
}

#if JUCE_MAC
static bool encodeAAC(const juce::AudioBuffer<float>& buf, double sr, int kbps, const juce::File& out)
{
    out.deleteFile();
    AudioStreamBasicDescription dst {};
    dst.mSampleRate       = sr;
    dst.mFormatID         = kAudioFormatMPEG4AAC;
    dst.mChannelsPerFrame = 2;

    const auto pathUtf8 = out.getFullPathName().toRawUTF8();
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        nullptr, (const UInt8*) pathUtf8, (CFIndex) strlen(pathUtf8), false);
    ExtAudioFileRef ea = nullptr;
    OSStatus st = ExtAudioFileCreateWithURL(url, kAudioFileM4AType, &dst,
                                            nullptr, kAudioFileFlags_EraseFile, &ea);
    CFRelease(url);
    if (st != noErr || ea == nullptr) return false;

    AudioStreamBasicDescription client {};
    client.mSampleRate       = sr;
    client.mFormatID         = kAudioFormatLinearPCM;
    client.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked
                             | kAudioFormatFlagIsNonInterleaved;
    client.mBitsPerChannel   = 32;
    client.mChannelsPerFrame = 2;
    client.mFramesPerPacket  = 1;
    client.mBytesPerFrame    = 4;
    client.mBytesPerPacket   = 4;
    st = ExtAudioFileSetProperty(ea, kExtAudioFileProperty_ClientDataFormat,
                                 sizeof(client), &client);
    if (st == noErr)
    {
        // Bitrate on the underlying converter, then resync the file
        AudioConverterRef conv = nullptr;
        UInt32 sz = sizeof(conv);
        if (ExtAudioFileGetProperty(ea, kExtAudioFileProperty_AudioConverter, &sz, &conv) == noErr
            && conv != nullptr)
        {
            UInt32 br = (UInt32) kbps * 1000;
            AudioConverterSetProperty(conv, kAudioConverterEncodeBitRate, sizeof(br), &br);
            CFArrayRef cfg = nullptr;
            ExtAudioFileSetProperty(ea, kExtAudioFileProperty_ConverterConfig, sizeof(cfg), &cfg);
        }
        struct { AudioBufferList list; ::AudioBuffer second; } abl {};
        const int n = buf.getNumSamples();
        for (int pos = 0; pos < n && st == noErr; pos += 4096)
        {
            const UInt32 len = (UInt32) juce::jmin(4096, n - pos);
            abl.list.mNumberBuffers = 2;
            abl.list.mBuffers[0] = { 1, len * 4, (void*) (buf.getReadPointer(0) + pos) };
            abl.second           = { 1, len * 4, (void*) (buf.getReadPointer(1) + pos) };
            st = ExtAudioFileWrite(ea, len, &abl.list);
        }
    }
    ExtAudioFileDispose(ea);
    return st == noErr;
}
#endif

Result render(const juce::String& sourcePath, const Preset& preset, bool normalise)
{
    Result res;
    juce::File src(sourcePath);
    if (!src.existsAsFile()) { res.error = "Source file not found"; return res; }

    const juce::String key = juce::String::toHexString(
        (sourcePath + "|" + juce::String(src.getLastModificationTime().toMilliseconds())
         + "|" + preset.id + "|" + (normalise ? "n14" : "raw")).hashCode64());
    auto origOut = cacheDir().getChildFile(key + "_orig.wav");
    auto rendOut = cacheDir().getChildFile(key + "_rend.wav");

    juce::AudioBuffer<float> srcBuf;
    double sr = 44100.0;

    if (origOut.existsAsFile() && rendOut.existsAsFile())
    {
        // Cache hit: thumbnails rebuilt from the cached files (fast)
        juce::AudioBuffer<float> a, b; double s2;
        if (readWholeFile(origOut.getFullPathName(), a, s2)
            && readWholeFile(rendOut.getFullPathName(), b, s2))
        {
            origOut.setLastModificationTime(juce::Time::getCurrentTime());  // LRU touch
            rendOut.setLastModificationTime(juce::Time::getCurrentTime());
            res.ok = true; res.cacheHit = true;
            res.originalWav = origOut.getFullPathName();
            res.renderWav   = rendOut.getFullPathName();
            res.origThumb   = makeThumb(a);
            res.renderThumb = makeThumb(b);
            EchoJay_NSLog(("EJCodec: cache hit " + juce::String(preset.id)).toRawUTF8());
            return res;
        }
    }

    if (!readWholeFile(sourcePath, srcBuf, sr)) { res.error = "Could not read source"; return res; }

    // Encode the UNGAINED master (codec sees it exactly as distribution would)
    auto encFile = cacheDir().getChildFile(key + (preset.isAAC ? ".m4a" : ".ogg"));
    bool encOk = false;
#if JUCE_MAC
    if (preset.isAAC) encOk = encodeAAC(srcBuf, sr, preset.kbps, encFile);
#endif
    if (!preset.isAAC) encOk = encodeOgg(srcBuf, sr, preset.kbps, encFile);
    if (!encOk) { res.error = "Encode failed"; return res; }

    juce::AudioBuffer<float> decBuf;
    double decSr = sr;
    const bool decOk = readWholeFile(encFile.getFullPathName(), decBuf, decSr);
    encFile.deleteFile();
    if (!decOk) { res.error = "Decode failed"; return res; }

    // ONE gain from the SOURCE's integrated LUFS, applied to both sides
    if (normalise)
    {
        const float lufs = measureIntegratedLufs(srcBuf, sr);
        if (lufs > -90.0f)
        {
            const float gain = juce::Decibels::decibelsToGain(-14.0f - lufs);
            srcBuf.applyGain(gain);
            decBuf.applyGain(gain);
        }
    }

    if (!writeWav(origOut, srcBuf, sr) || !writeWav(rendOut, decBuf, decSr))
    { res.error = "Could not write render"; return res; }
    evictCache();

    res.ok = true;
    res.originalWav = origOut.getFullPathName();
    res.renderWav   = rendOut.getFullPathName();
    res.origThumb   = makeThumb(srcBuf);
    res.renderThumb = makeThumb(decBuf);
    EchoJay_NSLog(("EJCodec: rendered " + juce::String(preset.id)
                   + " norm=" + juce::String((int) normalise)
                   + " src=" + src.getFileName()).toRawUTF8());
    return res;
}

} // namespace CodecRender
