// Haiku (air/OS) audio, all through the Media Kit:
//   playback  BMediaFile/BMediaTrack decode to raw float frames (any codec the
//             system's media add-ons have: MP3, AAC/M4A, Vorbis, Opus, FLAC,
//             …) and a BSoundPlayer pulls them on its own thread, into the
//             system mixer (which resamples to the output device).
//   capture   BMediaRecorder on the default audio input; whatever format the
//             input delivers is downmixed and resampled to 16 kHz mono s16le
//             on the recorder node's thread.
//   sounds    the system sound events of the Sounds preferences (BMediaFiles
//             "Sounds"), played by a short-lived thread with its own player.
//
// The Media Kit calls our buffer hooks on its node threads: they only touch
// state behind a mutex, and everything for the app goes through App::post()
// with a liveness token, so a callback never reaches a destroyed object.
#include "audio/audio_internal.h"

#include "plat/plat.h"

#include <Entry.h>
#include <MediaDefs.h>
#include <MediaFile.h>
#include <MediaFiles.h>
#include <MediaRecorder.h>
#include <MediaRoster.h>
#include <MediaTrack.h>
#include <OS.h>
#include <SoundPlayer.h>
#include <String.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace plat::audio {

namespace {

// $PLAT_AUDIO_HELPERS set (tests): no device is ever opened, nothing plays,
// as on macOS.
bool nullAudio() {
    const char *v = std::getenv("PLAT_AUDIO_HELPERS");
    return v && *v;
}

std::string errorText(status_t err) {
    const char *s = std::strerror(err);
    return s ? s : "";
}

// ── Sample formats ──────────────────────────────────────────────────────────

size_t sampleSize(uint32 format) {
    return format & media_raw_audio_format::B_AUDIO_SIZE_MASK;
}

bool knownFormat(uint32 format) {
    switch (format) {
    case media_raw_audio_format::B_AUDIO_FLOAT:
    case media_raw_audio_format::B_AUDIO_INT:
    case media_raw_audio_format::B_AUDIO_SHORT:
    case media_raw_audio_format::B_AUDIO_UCHAR:
    case media_raw_audio_format::B_AUDIO_CHAR:
        return true;
    default:
        return false;
    }
}

// Sample `p` of raw format `format` as -1..1; `swap` when its byte order is
// not the host's.
float sampleAt(const char *p, uint32 format, bool swap) {
    unsigned char b[4];
    const size_t  n = sampleSize(format);
    std::memcpy(b, p, std::min<size_t>(n, 4)); // samples need not be aligned
    if (swap)
        std::reverse(b, b + n);
    switch (format) {
    case media_raw_audio_format::B_AUDIO_FLOAT: {
        float v;
        std::memcpy(&v, b, 4);
        return std::isfinite(v) ? v : 0.0f;
    }
    case media_raw_audio_format::B_AUDIO_INT: {
        int32 v;
        std::memcpy(&v, b, 4);
        return float(double(v) / 2147483648.0);
    }
    case media_raw_audio_format::B_AUDIO_SHORT: {
        int16 v;
        std::memcpy(&v, b, 2);
        return float(v) / 32768.0f;
    }
    case media_raw_audio_format::B_AUDIO_UCHAR:
        return (float(b[0]) - 128.0f) / 128.0f;
    case media_raw_audio_format::B_AUDIO_CHAR:
        return float(int8(b[0])) / 128.0f;
    default:
        return 0.0f;
    }
}

void storeSample(char *p, uint32 format, float v) {
    v = std::clamp(v, -1.0f, 1.0f);
    switch (format) {
    case media_raw_audio_format::B_AUDIO_FLOAT:
        std::memcpy(p, &v, 4);
        break;
    case media_raw_audio_format::B_AUDIO_INT: {
        const auto s = int32(std::lrint(double(v) * 2147483647.0));
        std::memcpy(p, &s, 4);
        break;
    }
    case media_raw_audio_format::B_AUDIO_SHORT: {
        const auto s = int16(std::lrintf(v * 32767.0f));
        std::memcpy(p, &s, 2);
        break;
    }
    case media_raw_audio_format::B_AUDIO_UCHAR:
        *p = char(uint8(std::lrintf(128.0f + v * 127.0f)));
        break;
    case media_raw_audio_format::B_AUDIO_CHAR:
        *p = char(int8(std::lrintf(v * 127.0f)));
        break;
    default:
        break;
    }
}

void fillSilence(void *buffer, size_t size, uint32 format) {
    // Unsigned 8-bit audio is centred on 0x80, everything else on zero.
    std::memset(buffer, format == media_raw_audio_format::B_AUDIO_UCHAR ? 0x80 : 0, size);
}

// ── Decoding ────────────────────────────────────────────────────────────────

// The first decodable audio track of a file, read as interleaved float
// frames. Not thread safe: its users serialize access.
class TrackReader {
public:
    enum class Read : uint8_t { Ok, End, Failed };

    static std::unique_ptr<TrackReader> open(const entry_ref &ref, Failure *failure) {
        auto *file = new BMediaFile(&ref);
        if (const status_t err = file->InitCheck(); err != B_OK) {
            delete file;
            *failure = {Error::Unsupported, errorText(err)};
            return nullptr;
        }
        for (int32 i = 0; i < file->CountTracks(); ++i) {
            BMediaTrack *track = file->TrackAt(i);
            if (!track)
                continue;
            media_format encoded;
            if (track->InitCheck() == B_OK && track->EncodedFormat(&encoded) == B_OK &&
                encoded.IsAudio()) {
                // Ask for float; the decoder answers with its best fit, which
                // read() converts from if it is something else.
                media_format decoded;
                decoded.type                   = B_MEDIA_RAW_AUDIO;
                decoded.u.raw_audio            = media_raw_audio_format::wildcard;
                decoded.u.raw_audio.format     = media_raw_audio_format::B_AUDIO_FLOAT;
                decoded.u.raw_audio.byte_order = B_MEDIA_HOST_ENDIAN;
                const media_raw_audio_format &raw = decoded.u.raw_audio;
                if (track->DecodedFormat(&decoded) == B_OK && decoded.type == B_MEDIA_RAW_AUDIO &&
                    raw.channel_count > 0 && raw.frame_rate > 0 && knownFormat(raw.format))
                    return std::unique_ptr<TrackReader>(new TrackReader(file, track, raw));
            }
            file->ReleaseTrack(track);
        }
        delete file;
        *failure = {Error::Unsupported, {}};
        return nullptr;
    }

    ~TrackReader() {
        _file->ReleaseTrack(_track);
        delete _file;
    }

    int     channels() const { return _channels; }
    double  rate() const { return _rate; }
    int64_t frame() const { return _frame; }
    int64_t durationMs() const { return _durationUs / 1000; }

    // Up to `frames` frames into `out` (frames × channels() floats); *got is
    // how many came. End and Failed may still come with some frames.
    Read read(float *out, int64_t frames, int64_t *got) {
        *got = 0;
        while (*got < frames) {
            if (_bufPos >= _bufFrames) {
                if (_eof)
                    return Read::End;
                int64         count = 0;
                const status_t err   = _track->ReadFrames(_buf.data(), &count);
                if (count <= 0) {
                    // B_LAST_BUFFER_ERROR is the normal end; some decoders
                    // report other errors on their last packet instead.
                    _eof = err == B_OK || err == B_LAST_BUFFER_ERROR || nearEnd();
                    return _eof ? Read::End : Read::Failed;
                }
                _bufFrames = std::min<int64_t>(count, int64_t(_buf.size() / _frameBytes));
                _bufPos    = 0;
            }
            const int64_t n   = std::min(frames - *got, _bufFrames - _bufPos);
            const char   *src = _buf.data() + size_t(_bufPos) * _frameBytes;
            float        *dst = out + size_t(*got) * size_t(_channels);
            const size_t  ss  = sampleSize(_format);
            for (int64_t i = 0; i < n * _channels; ++i)
                dst[i] = _format == media_raw_audio_format::B_AUDIO_FLOAT && !_swap
                             ? loadFloat(src + size_t(i) * 4)
                             : sampleAt(src + size_t(i) * ss, _format, _swap);
            _bufPos += n;
            *got += n;
            _frame += n;
        }
        return Read::Ok;
    }

    void seekMs(int64_t ms) {
        int64 target = std::max<int64>(0, int64(double(ms) * _rate / 1000.0));
        if (_totalFrames > 0)
            target = std::min<int64>(target, _totalFrames);
        int64 frame = target;
        if (_track->SeekToFrame(&frame, B_MEDIA_SEEK_CLOSEST_BACKWARD) != B_OK) {
            bigtime_t t = bigtime_t(double(target) * 1e6 / _rate);
            if (_track->SeekToTime(&t, B_MEDIA_SEEK_CLOSEST_BACKWARD) != B_OK)
                return; // stays where it was
            frame = int64(double(t) * _rate / 1e6);
        }
        _frame  = frame;
        _bufPos = _bufFrames = 0;
        _eof                 = false;
    }

private:
    TrackReader(BMediaFile *file, BMediaTrack *track, const media_raw_audio_format &raw)
        : _file(file),
          _track(track),
          _format(raw.format),
          _swap(raw.byte_order != 0 && raw.byte_order != B_MEDIA_HOST_ENDIAN),
          _channels(int(raw.channel_count)),
          _rate(raw.frame_rate),
          _frameBytes(sampleSize(raw.format) * raw.channel_count),
          _totalFrames(track->CountFrames()),
          _durationUs(std::max<bigtime_t>(0, track->Duration())) {
        // ReadFrames() writes a whole decoder buffer each time: room for one,
        // as negotiated (or a generous second of audio when it said nothing).
        size_t bytes = raw.buffer_size;
        if (bytes < _frameBytes)
            bytes = size_t(std::max(1.0f, raw.frame_rate)) * _frameBytes;
        _buf.resize(bytes);
    }

    static float loadFloat(const char *p) {
        float v;
        std::memcpy(&v, p, 4);
        return std::isfinite(v) ? v : 0.0f;
    }

    // Within half a second of the end the track claims to have.
    bool nearEnd() const {
        return _totalFrames > 0 && _frame + int64_t(_rate / 2) >= _totalFrames;
    }

    BMediaFile       *_file;
    BMediaTrack      *_track;
    uint32            _format;
    bool              _swap;
    int               _channels;
    double            _rate;
    size_t            _frameBytes;
    int64_t           _totalFrames;
    bigtime_t         _durationUs;
    std::vector<char> _buf;
    int64_t           _bufFrames = 0, _bufPos = 0;
    int64_t           _frame     = 0; // frames handed out (the play position)
    bool              _eof       = false;
};

// Converts `frames` interleaved float frames of `inCh` channels into the
// output buffer's format and channel layout.
void writeFrames(
    const float *in, int inCh, int64_t frames, char *out, const media_raw_audio_format &format
) {
    const int    outCh = int(format.channel_count);
    const size_t ss    = sampleSize(format.format);
    for (int64_t f = 0; f < frames; ++f) {
        const float *src = in + size_t(f) * size_t(inCh);
        float        mono = 0;
        if (outCh == 1 && inCh > 1) {
            for (int c = 0; c < inCh; ++c)
                mono += src[c];
            mono /= float(inCh);
        }
        for (int c = 0; c < outCh; ++c) {
            float v;
            if (outCh == 1 && inCh > 1)
                v = mono;
            else if (inCh == 1)
                v = src[0];
            else
                v = c < inCh ? src[c] : 0.0f;
            storeSample(out + (size_t(f) * size_t(outCh) + size_t(c)) * ss, format.format, v);
        }
    }
}

// What a sound player pulls from: a reader behind a lock, filling buffers on
// the player's thread. `ended` once the reader ran out or failed; after that
// the player gets silence.
struct Stream {
    std::mutex                   m;
    std::unique_ptr<TrackReader> reader;
    std::vector<float>           scratch;
    bool                         ended = false;

    // Fills one output buffer; the reader's verdict when it stopped early.
    TrackReader::Read fill(void *buffer, size_t size, const media_raw_audio_format &format) {
        const size_t frameBytes = sampleSize(format.format) * format.channel_count;
        if (ended || !reader || frameBytes == 0 || !knownFormat(format.format)) {
            fillSilence(buffer, size, format.format);
            return TrackReader::Read::Ok;
        }
        const int64_t frames = int64_t(size / frameBytes);
        const int     inCh   = reader->channels();
        scratch.resize(size_t(frames) * size_t(inCh)); // grows once, to the buffer size
        int64_t                 got = 0;
        const TrackReader::Read r   = reader->read(scratch.data(), frames, &got);
        writeFrames(scratch.data(), inCh, got, static_cast<char *>(buffer), format);
        if (size_t(got) * frameBytes < size)
            fillSilence(
                static_cast<char *>(buffer) + size_t(got) * frameBytes,
                size - size_t(got) * frameBytes,
                format.format
            );
        if (r != TrackReader::Read::Ok)
            ended = true;
        return r;
    }
};

// A BSoundPlayer asking for the reader's own rate and channels as float (the
// mixer converts from there). Null with *err set when it can't connect.
BSoundPlayer *makeSoundPlayer(
    const TrackReader &reader, const char *name, BSoundPlayer::BufferPlayerFunc fn, void *cookie,
    status_t *err
) {
    media_raw_audio_format format = media_raw_audio_format::wildcard;
    format.frame_rate             = float(reader.rate());
    format.channel_count          = uint32(reader.channels());
    format.format                 = media_raw_audio_format::B_AUDIO_FLOAT;
    format.byte_order             = B_MEDIA_HOST_ENDIAN;
    auto *player                  = new BSoundPlayer(&format, name, fn, nullptr, cookie);
    *err                          = player->InitCheck();
    if (*err != B_OK) {
        delete player;
        return nullptr;
    }
    return player;
}

// Stopped and gone: once Stop(true) returns the node no longer calls the
// buffer function, so whatever its cookie points at may then be freed.
void destroySoundPlayer(BSoundPlayer *player) {
    if (!player)
        return;
    player->Stop(true, true);
    delete player;
}

bool refForPath(const std::string &path, entry_ref *ref) {
    return !path.empty() && get_ref_for_path(path.c_str(), ref) == B_OK;
}

// ── Playback ────────────────────────────────────────────────────────────────

class HaikuPlayer;

// Lets posted events find their player; `owner` is cleared (App thread) when
// the player goes, so later deliveries are dropped.
struct PlayerToken {
    HaikuPlayer *owner = nullptr;
};

enum class PlayerEv : uint8_t { Loaded, Ended, Failed };

// The sound player's cookie: the stream plus what its thread needs to post.
struct PlayerStream : Stream {
    App                         *app = nullptr;
    std::shared_ptr<PlayerToken> token;
    uint64_t                     gen = 0;
};

class HaikuPlayer final : public Player {
public:
    explicit HaikuPlayer(App &app) : _app(app) { _token->owner = this; }
    ~HaikuPlayer() override {
        _token->owner = nullptr;
        release();
    }

    void load(const std::string &path) override {
        release();
        const uint64_t gen = ++_gen;
        if (nullAudio()) {
            post(_app, _token, gen, PlayerEv::Failed, {Error::Unavailable, {}});
            return;
        }
        Failure   failure;
        entry_ref ref;
        if (!refForPath(path, &ref)) {
            post(_app, _token, gen, PlayerEv::Failed, {Error::Unsupported, {}});
            return;
        }
        std::unique_ptr<TrackReader> reader = TrackReader::open(ref, &failure);
        if (!reader) {
            post(_app, _token, gen, PlayerEv::Failed, failure);
            return;
        }
        auto stream    = std::make_unique<PlayerStream>();
        stream->reader = std::move(reader);
        stream->app    = &_app;
        stream->token  = _token;
        stream->gen    = gen;
        status_t err   = B_OK;
        _sound = makeSoundPlayer(*stream->reader, "msga player", &playBuffer, stream.get(), &err);
        if (!_sound) {
            post(_app, _token, gen, PlayerEv::Failed, {Error::Unavailable, errorText(err)});
            return;
        }
        _durationMs = stream->reader->durationMs();
        _stream     = std::move(stream);
        post(_app, _token, gen, PlayerEv::Loaded, {});
    }

    void play() override {
        if (!_sound || _started)
            return;
        bool ended;
        {
            std::lock_guard lock(_stream->m);
            ended = _stream->ended;
        }
        if (ended) // played out: play() starts over, as the other backends do
            seek(0);
        if (const status_t err = _sound->Start(); err != B_OK) {
            post(_app, _token, _gen, PlayerEv::Failed, {Error::Unavailable, errorText(err)});
            return;
        }
        _started = true;
    }

    void pause() override {
        // Not blocking: what the mixer has queued (a few ms) still plays out.
        if (_sound && _started)
            _sound->Stop(false, true);
        _started = false;
    }

    void seek(int64_t ms) override {
        if (!_stream)
            return;
        std::lock_guard lock(_stream->m);
        _stream->reader->seekMs(std::max<int64_t>(0, ms));
        _stream->ended = false;
    }

    void stop() override {
        ++_gen;
        release();
    }

    int64_t positionMs() const override {
        if (!_stream)
            return 0;
        std::lock_guard lock(_stream->m);
        const TrackReader &r  = *_stream->reader;
        const int64_t      ms = int64_t(double(r.frame()) * 1000.0 / r.rate());
        return _durationMs > 0 ? std::min(ms, _durationMs) : ms;
    }
    int64_t durationMs() const override { return _stream ? _durationMs : 0; }

private:
    // On the sound player's thread.
    static void
    playBuffer(void *cookie, void *buffer, size_t size, const media_raw_audio_format &f) {
        auto             &s = *static_cast<PlayerStream *>(cookie);
        TrackReader::Read r;
        {
            std::lock_guard lock(s.m);
            if (s.ended) {
                fillSilence(buffer, size, f.format);
                return;
            }
            r = s.fill(buffer, size, f);
        }
        if (r == TrackReader::Read::End)
            post(*s.app, s.token, s.gen, PlayerEv::Ended, {});
        else if (r == TrackReader::Read::Failed)
            post(*s.app, s.token, s.gen, PlayerEv::Failed, {Error::PlaybackFailed, {}});
    }

    // From any thread; delivered on the App's thread, unless the media it is
    // about has been replaced (or the player is gone) by then.
    static void post(
        App &app, const std::shared_ptr<PlayerToken> &token, uint64_t gen, PlayerEv ev, Failure f
    ) {
        app.post([token, gen, ev, f = std::move(f)] {
            HaikuPlayer *p = token->owner;
            if (!p || gen != p->_gen)
                return;
            switch (ev) {
            case PlayerEv::Loaded:
                if (p->onLoaded)
                    p->onLoaded();
                break;
            case PlayerEv::Ended:
                p->pause(); // releases the node; the position stays at the end
                if (p->onEnded)
                    p->onEnded();
                break;
            case PlayerEv::Failed:
                p->pause();
                if (p->onFailed)
                    p->onFailed(f);
                break;
            }
        });
    }

    // The sound player goes before the stream it reads from.
    void release() {
        destroySoundPlayer(_sound);
        _sound      = nullptr;
        _started    = false;
        _durationMs = 0;
        _stream.reset();
    }

    App                          &_app;
    std::shared_ptr<PlayerToken>  _token = std::make_shared<PlayerToken>();
    std::unique_ptr<PlayerStream> _stream;
    BSoundPlayer                 *_sound      = nullptr;
    int64_t                       _durationMs = 0;
    uint64_t                      _gen        = 0;
    bool                          _started    = false;
};

// ── Capture ─────────────────────────────────────────────────────────────────

constexpr int    kLevelIntervalMs = 50;   // onLevel at ~20 Hz
constexpr int    kFirstDataMs     = 3000; // input connected but silent this long = broken
constexpr size_t kLevelWindowBytes =
    size_t(Recorder::kSampleRate) * Recorder::kChannels * 2 * kLevelIntervalMs / 1000;

class HaikuRecorder;

struct RecorderToken {
    HaikuRecorder *owner = nullptr; // App thread only; null once the recorder is gone
};

enum class RecEv : uint8_t { Started, Level };

// What the recorder node's thread writes: the take as 16 kHz mono s16le after
// kWavHeader free bytes, and the downmix/resampler state between buffers.
struct Capture {
    std::mutex                     m;
    std::string                    pcm       = std::string(kWavHeader, '\0');
    size_t                         levelFrom = kWavHeader;
    bool                           gotData = false, closed = false;
    // Box-filter resampler: input samples are summed until an output sample
    // is due (phase counts in units of output rate × input samples).
    int64_t                        phase = 0;
    float                          acc   = 0;
    int                            accN  = 0;
    App                           *app   = nullptr;
    std::shared_ptr<RecorderToken> token;
    uint64_t                       session = 0;

    void emit(float v) {
        const auto s = int16(std::lrintf(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
        const char b[2] = {char(uint16(s) & 0xff), char(uint16(s) >> 8)};
        pcm.append(b, 2);
    }

    void push(float mono, int64_t inRate) {
        acc += mono;
        ++accN;
        phase += Recorder::kSampleRate;
        bool emitted = false;
        while (phase >= inRate) { // more than once only when the input is below 16 kHz
            phase -= inRate;
            emit(acc / float(accN));
            emitted = true;
        }
        if (emitted)
            acc = 0, accN = 0;
    }
};

class HaikuRecorder final : public Recorder {
public:
    explicit HaikuRecorder(App &app) : _app(app) { _token->owner = this; }
    ~HaikuRecorder() override {
        _token->owner = nullptr;
        cancel();
    }

    void start() override {
        if (_recording)
            return;
        _recording         = true;
        const uint64_t ses = ++_session;
        if (nullAudio()) {
            fail(ses, {Error::NoMicrophone, {}});
            return;
        }
        status_t      err    = B_OK;
        BMediaRoster *roster = BMediaRoster::Roster(&err);
        if (!roster) {
            fail(ses, {Error::StartFailed, errorText(err)});
            return;
        }
        if (roster->GetAudioInput(&_input) != B_OK || _input.node <= 0) {
            fail(ses, {Error::NoMicrophone, {}});
            return;
        }
        _haveInput = true;
        media_output output;
        if (!freeOutput(*roster, output)) {
            teardown();
            fail(ses, {Error::MicrophoneBusy, {}});
            return;
        }
        _cap          = std::make_shared<Capture>();
        _cap->app     = &_app;
        _cap->token   = _token;
        _cap->session = ses;
        _rec          = new BMediaRecorder("msga voice input", B_MEDIA_RAW_AUDIO);
        err           = _rec->InitCheck();
        if (err == B_OK) {
            _rec->SetHooks(&recordBuffer, nullptr, _cap.get());
            // Whatever the input produces; recordBuffer() converts it.
            media_format format;
            format.type        = B_MEDIA_RAW_AUDIO;
            format.u.raw_audio = media_raw_audio_format::wildcard;
            err                = _rec->Connect(_input, &output, &format);
            if (err == B_OK)
                err = _rec->Start();
        }
        if (err != B_OK) {
            teardown();
            fail(ses, {Error::StartFailed, errorText(err)});
            return;
        }
        // An input that connects but never delivers (a device with no
        // signal path) would otherwise record nothing forever.
        _firstData = _app.addTimer(kFirstDataMs, false, [this, ses] {
            _firstData = 0;
            if (ses != _session || !_cap)
                return;
            bool got;
            {
                std::lock_guard lock(_cap->m);
                got = _cap->gotData;
            }
            if (got)
                return;
            teardown();
            _recording = false;
            ++_session;
            if (onFailed)
                onFailed({Error::NoAudio, {}});
        });
    }

    void stop() override {
        if (!_recording)
            return;
        std::shared_ptr<Capture> cap = _cap;
        teardown();
        _recording = false;
        std::string wav;
        if (cap) {
            std::lock_guard lock(cap->m);
            wav.swap(cap->pcm);
        }
        if (wav.size() < kWavHeader)
            wav.assign(kWavHeader, '\0');
        wav.resize(wav.size() & ~size_t(1)); // whole samples only (the header is even)
        wavInPlace(wav, kSampleRate, kChannels);
        const uint64_t ses = ++_session; // drops level/started posts still queued
        _app.post([token = _token, ses, wav = std::move(wav)]() mutable {
            HaikuRecorder *r = token->owner;
            if (r && ses == r->_session && r->onFinished)
                r->onFinished(std::move(wav));
        });
    }

    void cancel() override {
        ++_session;
        teardown();
        _recording = false;
    }

    bool isRecording() const override { return _recording; }

private:
    // On the recorder node's thread: one buffer from the input, in whatever
    // format the connection settled on.
    static void recordBuffer(
        void *cookie, bigtime_t, void *data, size_t size, const media_format &format
    ) {
        auto                         &c   = *static_cast<Capture *>(cookie);
        const media_raw_audio_format &raw = format.u.raw_audio;
        const int                     ch  = int(raw.channel_count);
        const size_t                  ss  = sampleSize(raw.format);
        const auto                    rate = int64_t(std::lround(raw.frame_rate));
        if (ch <= 0 || ss == 0 || rate <= 0 || !knownFormat(raw.format))
            return;
        const bool swap = ss > 1 && raw.byte_order != 0 && raw.byte_order != B_MEDIA_HOST_ENDIAN;
        const size_t       frames  = size / (ss * size_t(ch));
        const char        *p       = static_cast<const char *>(data);
        bool               started = false;
        std::vector<float> levels;
        {
            std::lock_guard lock(c.m);
            if (c.closed)
                return;
            for (size_t f = 0; f < frames; ++f) {
                float sum = 0;
                for (int k = 0; k < ch; ++k)
                    sum += sampleAt(p + (f * size_t(ch) + size_t(k)) * ss, raw.format, swap);
                c.push(sum / float(ch), rate);
            }
            if (frames > 0 && !c.gotData)
                c.gotData = started = true;
            for (; c.levelFrom + kLevelWindowBytes <= c.pcm.size();
                 c.levelFrom += kLevelWindowBytes)
                levels.push_back(peakOf(c.pcm.data() + c.levelFrom, kLevelWindowBytes));
        }
        if (started)
            post(c, RecEv::Started, 0);
        for (float level : levels)
            post(c, RecEv::Level, level);
    }

    static void post(const Capture &c, RecEv ev, float level) {
        c.app->post([token = c.token, ses = c.session, ev, level] {
            HaikuRecorder *r = token->owner;
            if (!r || ses != r->_session)
                return;
            if (ev == RecEv::Started) {
                if (r->onStarted)
                    r->onStarted();
            } else if (r->_recording && r->onLevel) {
                r->onLevel(level);
            }
        });
    }

    // Posted, so it never runs inside the caller's start().
    void fail(uint64_t ses, Failure f) {
        _recording = false;
        _app.post([token = _token, ses, f = std::move(f)] {
            HaikuRecorder *r = token->owner;
            if (r && ses == r->_session && r->onFailed)
                r->onFailed(f);
        });
    }

    // A free output of the input node. Outputs still connected to a team that
    // died (its consumer's port is gone) are reclaimed first, as the Media
    // preferences do.
    bool freeOutput(BMediaRoster &roster, media_output &output) {
        int32 count = 0;
        if (roster.GetFreeOutputsFor(_input, &output, 1, &count, B_MEDIA_RAW_AUDIO) == B_OK &&
            count > 0)
            return true;
        media_output outputs[16];
        if (roster.GetConnectedOutputsFor(_input, outputs, 16, &count) != B_OK)
            return false;
        bool cleared = false;
        for (int32 i = 0; i < count; ++i) {
            port_info info;
            if (get_port_info(outputs[i].destination.port, &info) == B_OK)
                continue;
            roster.Disconnect(_input.node, outputs[i].source, _input.node, outputs[i].destination);
            cleared = true;
        }
        count = 0;
        return cleared &&
               roster.GetFreeOutputsFor(_input, &output, 1, &count, B_MEDIA_RAW_AUDIO) == B_OK &&
               count > 0;
    }

    // Disconnects and releases everything; reports nothing.
    void teardown() {
        if (_firstData)
            _app.cancelTimer(_firstData);
        _firstData = 0;
        if (_cap) {
            std::lock_guard lock(_cap->m);
            _cap->closed = true;
        }
        if (_rec) {
            _rec->Stop();
            _rec->SetHooks(nullptr, nullptr, nullptr);
            if (_rec->IsConnected())
                _rec->Disconnect();
            delete _rec;
            _rec = nullptr;
        }
        _cap.reset();
        if (_haveInput) {
            if (BMediaRoster *roster = BMediaRoster::CurrentRoster()) {
                // BMediaRecorder started the input; stop it again when nothing
                // else uses it (a sound card's node often feeds the mixer too).
                media_output outputs[4];
                media_input  inputs[4];
                int32        outCount = 0, inCount = 0;
                if (roster->GetConnectedOutputsFor(_input, outputs, 4, &outCount) == B_OK &&
                    roster->GetConnectedInputsFor(_input, inputs, 4, &inCount) == B_OK &&
                    outCount == 0 && inCount == 0)
                    roster->StopNode(_input, 0, true);
                roster->ReleaseNode(_input);
            }
            _haveInput = false;
        }
    }

    App                           &_app;
    std::shared_ptr<RecorderToken> _token = std::make_shared<RecorderToken>();
    std::shared_ptr<Capture>       _cap;
    BMediaRecorder                *_rec = nullptr;
    media_node                     _input;
    bool                           _haveInput = false;
    TimerId                        _firstData = 0;
    uint64_t                       _session   = 0;
    bool                           _recording = false;
};

// ── Notification sounds ─────────────────────────────────────────────────────

constexpr int       kMaxSounds   = 4;              // playing at once; more are dropped
constexpr bigtime_t kMaxSoundUs  = 30 * 1000000LL; // a long file is cut off here
std::atomic<int>    g_soundsPlaying{0};

// The file the Sounds preferences assign to event `name`, if any.
bool systemSoundRef(const std::string &name, entry_ref *ref) {
    BMediaFiles files;
    *ref = entry_ref();
    return files.GetRefFor(BMediaFiles::B_SOUNDS, name.c_str(), ref) == B_OK && ref->name &&
           *ref->name;
}

// One sound to the end: its own reader and player, waited for on this thread.
struct SoundStream : Stream {
    sem_id done = -1;
};

void soundBuffer(void *cookie, void *buffer, size_t size, const media_raw_audio_format &f) {
    auto &s = *static_cast<SoundStream *>(cookie);
    bool  finished;
    {
        std::lock_guard lock(s.m);
        const bool before = s.ended;
        s.fill(buffer, size, f);
        finished = !before && s.ended;
    }
    if (finished)
        release_sem_etc(s.done, 1, B_DO_NOT_RESCHEDULE);
}

bool playRef(const entry_ref &ref) {
    Failure     failure;
    SoundStream s;
    s.reader = TrackReader::open(ref, &failure);
    if (!s.reader)
        return false;
    s.done = create_sem(0, "msga sound done");
    if (s.done < 0)
        return false;
    status_t      err    = B_OK;
    BSoundPlayer *player = makeSoundPlayer(*s.reader, "msga sound", &soundBuffer, &s, &err);
    bool          ok     = player && player->Start() == B_OK;
    if (ok) {
        const bigtime_t len = s.reader->durationMs() * 1000;
        const bigtime_t wait =
            len > 0 ? std::min(len + 2000000, kMaxSoundUs) : kMaxSoundUs;
        acquire_sem_etc(s.done, 1, B_RELATIVE_TIMEOUT, wait);
        // What the mixer still holds plays out before the node goes.
        snooze(player->Latency());
    }
    destroySoundPlayer(player);
    delete_sem(s.done);
    return ok;
}

struct SoundJob {
    std::string name, file;
};

status_t soundThread(void *p) {
    std::unique_ptr<SoundJob> job(static_cast<SoundJob *>(p));
    entry_ref                 ref;
    if (!(!job->name.empty() && systemSoundRef(job->name, &ref) && playRef(ref)) &&
        refForPath(job->file, &ref))
        playRef(ref);
    g_soundsPlaying.fetch_sub(1);
    return B_OK;
}

} // namespace

std::unique_ptr<Player> Player::create(App &app) {
    return std::make_unique<HaikuPlayer>(app);
}

bool canPlayExtension(std::string_view ext) {
    // What the stock media add-ons (the ffmpeg plugin, chiefly) decode.
    constexpr const char *k[] = {
        "mp3", "m4a", "mp4", "aac", "ogg", "oga", "opus", "wav", "flac",
        "webm", "mka", "aif", "aiff", "wma", "mp2", "ac3"
    };
    for (const char *e : k)
        if (ext == e)
            return true;
    return false;
}

std::unique_ptr<Recorder> createNativeRecorder(App &app) {
    return std::make_unique<HaikuRecorder>(app);
}

std::vector<SystemSound> systemSounds() {
    std::vector<SystemSound> out;
    BMediaFiles              files;
    if (files.RewindRefs(BMediaFiles::B_SOUNDS) != B_OK)
        return out;
    BString   name;
    entry_ref ref;
    while (files.GetNextRef(&name, &ref) == B_OK) {
        // Only events with a file assigned: the others would play the
        // fallback, which is not what picking them suggests.
        if (ref.name && *ref.name && name.Length() > 0)
            out.push_back({name.String(), name.String()});
        ref = entry_ref();
    }
    std::sort(out.begin(), out.end(), [](const SystemSound &a, const SystemSound &b) {
        return a.label < b.label;
    });
    return out;
}

void playSound(const std::string &name, const std::string &fallbackFile) {
    if (nullAudio() || (name.empty() && fallbackFile.empty()))
        return;
    if (g_soundsPlaying.fetch_add(1) >= kMaxSounds) {
        g_soundsPlaying.fetch_sub(1);
        return;
    }
    auto           *job = new SoundJob{name, fallbackFile};
    const thread_id t   = spawn_thread(soundThread, "msga sound", B_NORMAL_PRIORITY, job);
    if (t < 0 || resume_thread(t) != B_OK) {
        if (t >= 0)
            kill_thread(t);
        delete job;
        g_soundsPlaying.fetch_sub(1);
    }
}

} // namespace plat::audio
