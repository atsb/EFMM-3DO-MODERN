#include "sdl3_3do.h"
#include "castle.h"
#include "soundinterface.h"
#include "sound.h"
#include "app_proto.h"

#define MAX_SDL_VOICES 24
#define MAX_SDL_SOUNDS 128

typedef struct SDL3DOSample
{
    uint8 *data;
    int len;
    int freq;
    int channels;
} SDL3DOSample;

typedef struct SDL3DOVoice
{
    SDL_AudioStream *stream;
    int soundID;
    int active;
    SDL3DOSample sample;
} SDL3DOVoice;

static SDL_AudioDeviceID gAudioDevice;
static SDL3DOVoice gVoices[MAX_SDL_VOICES];
static SDL3DOVoice gMusic;
static SDL3DOSample gSounds[MAX_SDL_SOUNDS];
static int gAudioReady;
static int gMusicLoop;
static SDL_AudioStream *gMovieAudioStream;
static int gMovieAudioActive;
static uint64 gMovieAudioDurationUs;

static uint16 be16(const uint8 *p) { return (uint16)(((uint16)p[0] << 8) | p[1]); }
static uint32 fourcc(const uint8 *p) { return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) | ((uint32)p[2] << 8) | p[3]; }

static uint32 movie_be32(const uint8 *p) { return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) | ((uint32)p[2] << 8) | p[3]; }
static int decode_sdx2(const uint8 *src, int srcLen, int channels, uint8 **outData, int *outLen);

int SDL3_3DO_StartMovieAudio(const uint8 *data, size_t size)
{
    uint32_t position;
    uint32_t fileSize;
    uint32_t compressedSize = 0;
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    uint32_t bitDepth = 0;
    uint8 *compressed = NULL;
    uint8 *decoded = NULL;
    int decodedLen = 0;
    SDL_AudioSpec src;
    int haveHeader = 0;

    if (!gAudioReady || !data || size < 8u || size > 0xFFFFFFFFu)
        return 0;
    SDL3_3DO_StopMovieAudio();
    fileSize = (uint32_t)size;
    position = movie_be32(data + 4u);
    if (position < 8u || position > fileSize)
        position = 0u;

    while (position + 8u <= fileSize)
    {
        const uint8 *chunk = data + position;
        uint32_t chunkSize = movie_be32(chunk + 4u);
        if (chunkSize < 8u || position + chunkSize > fileSize)
            return 0;
        if (memcmp(chunk, "SNDS", 4) == 0 && chunkSize >= 64u &&
            memcmp(chunk + 16u, "SHDR", 4) == 0)
        {
            bitDepth = movie_be32(chunk + 40u);
            sampleRate = movie_be32(chunk + 44u);
            channels = movie_be32(chunk + 48u);
            if (movie_be32(chunk + 52u) != 0x53445832u)
                return 0;
            haveHeader = 1;
        }
        if (memcmp(chunk, "SNDS", 4) == 0 && chunkSize >= 24u &&
            memcmp(chunk + 16u, "SSMP", 4) == 0)
        {
            uint32_t blockSize = movie_be32(chunk + 20u);
            if (blockSize > chunkSize - 24u)
                return 0;
            compressedSize += blockSize;
        }
        position += chunkSize;
    }

    if (!haveHeader || !compressedSize || !sampleRate ||
        (channels != 1u && channels != 2u) || bitDepth != 16u)
        return 0;

    compressed = (uint8 *)malloc(compressedSize);
    if (!compressed)
        return 0;
    position = 0u;
    {
        uint32_t written = 0u;
        while (position + 8u <= fileSize && written < compressedSize)
        {
            const uint8 *chunk = data + position;
            uint32_t chunkSize = movie_be32(chunk + 4u);
            if (chunkSize < 8u || position + chunkSize > fileSize)
            {
                free(compressed);
                return 0;
            }
            if (memcmp(chunk, "SNDS", 4) == 0 && chunkSize >= 24u &&
                memcmp(chunk + 16u, "SSMP", 4) == 0)
            {
                uint32_t blockSize = movie_be32(chunk + 20u);
                if (blockSize > chunkSize - 24u || blockSize > compressedSize - written)
                {
                    free(compressed);
                    return 0;
                }
                memcpy(compressed + written, chunk + 24u, blockSize);
                written += blockSize;
            }
            position += chunkSize;
        }
        if (written != compressedSize)
        {
            free(compressed);
            return 0;
        }
    }

    if (decode_sdx2(compressed, (int)compressedSize, (int)channels, &decoded, &decodedLen) < 0)
    {
        free(compressed);
        return 0;
    }
    free(compressed);

    memset(&src, 0, sizeof(src));
    src.format = SDL_AUDIO_S16LE;
    src.channels = (int)channels;
    src.freq = (int)sampleRate;
    gMovieAudioStream = SDL_CreateAudioStream(&src, NULL);
    if (!gMovieAudioStream)
    {
        free(decoded);
        return 0;
    }
    if (!SDL_BindAudioStream(gAudioDevice, gMovieAudioStream) ||
        !SDL_PutAudioStreamData(gMovieAudioStream, decoded, decodedLen) ||
        !SDL_FlushAudioStream(gMovieAudioStream))
    {
        SDL_UnbindAudioStream(gMovieAudioStream);
        SDL_DestroyAudioStream(gMovieAudioStream);
        gMovieAudioStream = NULL;
        free(decoded);
        return 0;
    }
    free(decoded);
    gMovieAudioActive = 1;
    gMovieAudioDurationUs = ((uint64)(uint32_t)decodedLen * 1000000u) /
                            ((uint64)sampleRate * channels * (uint32_t)sizeof(int16));
    return 1;
}

int SDL3_3DO_IsMovieAudioPlaying(void)
{
    if (!gMovieAudioStream || !gMovieAudioActive)
        return 0;
    if (SDL_GetAudioStreamQueued(gMovieAudioStream) == 0 &&
        SDL_GetAudioStreamAvailable(gMovieAudioStream) == 0)
    {
        SDL3_3DO_StopMovieAudio();
        return 0;
    }
    return 1;
}

void SDL3_3DO_StopMovieAudio(void)
{
    if (gMovieAudioStream)
    {
        SDL_UnbindAudioStream(gMovieAudioStream);
        SDL_DestroyAudioStream(gMovieAudioStream);
        gMovieAudioStream = NULL;
    }
    gMovieAudioActive = 0;
    gMovieAudioDurationUs = 0;
}

uint64 SDL3_3DO_GetMovieAudioDurationUs(void)
{
    return gMovieAudioDurationUs;
}

static int16 sdx2_delta[256];
static int sdx2_table_ready;

static void init_sdx2_table(void)
{
    int i, v;
    if (sdx2_table_ready)
        return;
    for (i = -128; i <= 127; ++i)
    {
        v = i * i * 2;
        if (i < 0)
            v = -v;
        sdx2_delta[i + 128] = (int16)v;
    }
    sdx2_table_ready = 1;
}

static int decode_sdx2(const uint8 *src, int srcLen, int channels, uint8 **outData, int *outLen)
{
    int16 *dst;
    int i, ch;
    int32 pred[2] = {0, 0};

    if (!src || srcLen <= 0 || !outData || !outLen)
        return -1;
    if (channels < 1 || channels > 2)
        return -1;

    init_sdx2_table();
    dst = (int16 *)malloc((size_t)srcLen * sizeof(int16));
    if (!dst)
        return -1;

    for (i = 0; i < srcLen; ++i)
    {
        int8 code = (int8)src[i];
        ch = (channels == 2) ? (i & 1) : 0;
        if ((code & 1) == 0)
            pred[ch] = 0;
        pred[ch] += sdx2_delta[(int)code + 128];
        if (pred[ch] > 32767)
            pred[ch] = 32767;
        else if (pred[ch] < -32768)
            pred[ch] = -32768;
        dst[i] = (int16)pred[ch];
    }

    *outData = (uint8 *)dst;
    *outLen = srcLen * (int)sizeof(int16);
    return 0;
}

static void freesample(SDL3DOSample *s)
{
    if (s->data)
        free(s->data);
    memset(s, 0, sizeof(*s));
}

static int load_aifc(const char *filename, SDL3DOSample *out)
{
    uint8 *file = NULL, *p, *end, *ssndData = NULL;
    size_t len = 0;
    uint32 typ, comp = 0;
    int channels = 1, bits = 16, freq = 11025, dataLen = 0;
    int i;

    if (!filename || !out)
        return -1;
    if (!SDL3_3DO_LoadFile(filename, &file, &len))
    {
        fprintf(stderr, "SDL3 audio: cannot load %s (searched mounted 3DO image and disk)\n", filename);
        return -1;
    }
    if (len == 0 || len > 64 * 1024 * 1024)
    {
        fprintf(stderr, "SDL3 audio: invalid file size for %s\n", filename);
        free(file);
        return -1;
    }
    p = file;
    end = file + len;
    if (end - p < 12 || fourcc(p) != 0x464f524du)
    {
        fprintf(stderr, "SDL3 audio: %s is not a FORM file\n", filename);
        free(file);
        return -1;
    }
    typ = fourcc(p + 8);
    if (typ != 0x41494643u && typ != 0x41494646u)
    {
        fprintf(stderr, "SDL3 audio: %s is not AIFF/AIFC\n", filename);
        free(file);
        return -1;
    }
    p += 12;
    while (p + 8 <= end)
    {
        uint32 ct = fourcc(p), sz = ((uint32)p[4] << 24) | ((uint32)p[5] << 16) | ((uint32)p[6] << 8) | p[7];
        uint8 *q = p + 8;
        if (q > end || sz > (uint32)(end - q))
            break;
        if (ct == 0x434f4d4du && sz >= 18)
        {
            channels = be16(q);
            bits = be16(q + 6);

            {
                int sign = (q[8] & 0x80) ? -1 : 1, exp = ((q[8] & 0x7f) << 8) | q[9];
                uint64 mant = ((uint64)q[10] << 56) | ((uint64)q[11] << 48) | ((uint64)q[12] << 40) | ((uint64)q[13] << 32) | ((uint64)q[14] << 24) | ((uint64)q[15] << 16) | ((uint64)q[16] << 8) | q[17];
                double m = (double)mant / 9223372036854775808.0;
                double r = (exp ? ldexp(m, exp - 16383) : 0) * sign;
                if (r >= 1000 && r <= 192000)
                    freq = (int)(r + 0.5);
            }
            if (sz >= 22)
                comp = fourcc(q + 18);
        }
        else if (ct == 0x53534e44u && sz >= 8)
        {
            uint32 off = ((uint32)q[0] << 24) | ((uint32)q[1] << 16) | ((uint32)q[2] << 8) | q[3];
            if (off <= sz - 8)
            {
                ssndData = q + 8 + off;
                dataLen = (int)(sz - 8 - off);
            }
        }
        p = q + sz + (sz & 1);
    }

    if (!ssndData || dataLen <= 0)
    {
        fprintf(stderr, "SDL3 audio: %s has no usable SSND chunk\n", filename);
        free(file);
        return -1;
    }
    if (channels < 1 || channels > 2)
    {
        fprintf(stderr, "SDL3 audio: %s has unsupported channel count %d\n", filename, channels);
        free(file);
        return -1;
    }

    out->freq = freq;
    out->channels = channels;
    out->data = NULL;
    out->len = 0;

    if (comp == 0x53445832u)
    {
        int decodedLen = 0;
        if (bits != 16)
        {
            fprintf(stderr, "SDL3 audio: SDX2 %s advertises %d-bit samples; expected 16\n", filename, bits);
            free(file);
            return -1;
        }
        if (decode_sdx2(ssndData, dataLen, channels, &out->data, &decodedLen) < 0)
        {
            fprintf(stderr, "SDL3 audio: SDX2 decode failed for %s\n", filename);
            free(file);
            return -1;
        }
        out->len = decodedLen;
        fprintf(stderr, "SDL3 audio: loaded %s: SDX2 %d Hz, %d ch, %d compressed bytes -> %d PCM bytes\n", filename, freq, channels, dataLen, decodedLen);
        free(file);
        return 0;
    }

    if (comp && comp != 0x4e4f4e45u && comp != 0x74776f73u && comp != 0x736f7774u)
    {
        fprintf(stderr, "SDL3 audio: unsupported AIFC compression '%c%c%c%c' in %s\n",
                (int)((comp >> 24) & 255), (int)((comp >> 16) & 255), (int)((comp >> 8) & 255), (int)(comp & 255), filename);
        free(file);
        return -1;
    }

    if (bits == 16)
    {
        int n = dataLen / 2;
        int16 *d = (int16 *)malloc((size_t)n * 2);
        if (!d)
        {
            free(file);
            return -1;
        }
        for (i = 0; i < n; i++)
        {
            uint8 *s = ssndData + i * 2;
            uint16 v = (comp == 0x736f7774u) ? (uint16)(s[0] | ((uint16)s[1] << 8)) : be16(s);
            d[i] = (int16)v;
        }
        out->data = (uint8 *)d;
        out->len = n * 2;
    }
    else if (bits == 8)
    {
        int16 *d = (int16 *)malloc((size_t)dataLen * 2);
        if (!d)
        {
            free(file);
            return -1;
        }

        for (i = 0; i < dataLen; i++)
            d[i] = (int16)(((int)(int8)ssndData[i]) << 8);
        out->data = (uint8 *)d;
        out->len = dataLen * 2;
    }
    else
    {
        fprintf(stderr, "SDL3 audio: unsupported %d-bit PCM in %s\n", bits, filename);
        free(file);
        return -1;
    }

    fprintf(stderr, "SDL3 audio: loaded %s: PCM %d Hz, %d ch, %d-bit, %d bytes\n", filename, freq, channels, bits, out->len);
    free(file);
    return 0;
}

static void stop_voice(SDL3DOVoice *v)
{
    if (v->stream)
    {
        SDL_UnbindAudioStream(v->stream);
        SDL_DestroyAudioStream(v->stream);
        v->stream = NULL;
    }
    freesample(&v->sample);
    v->active = 0;
    v->soundID = 0;
}

static int make_voice_from_sample(int id, const SDL3DOSample *srcSample, int amp, int balance, int freqOverride)
{
    SDL_AudioSpec src;
    SDL3DOSample s;
    int i, sel = -1;
    if (!gAudioReady || !srcSample || !srcSample->data)
        return -1;
    for (i = 0; i < MAX_SDL_VOICES; i++)
        if (!gVoices[i].active)
        {
            sel = i;
            break;
        }
    if (sel < 0)
    {
        stop_voice(&gVoices[0]);
        sel = 0;
    }
    memset(&s, 0, sizeof(s));
    s.freq = srcSample->freq;
    s.channels = srcSample->channels;
    s.len = srcSample->len;
    s.data = (uint8 *)malloc((size_t)s.len);
    if (!s.data)
        return -1;
    memcpy(s.data, srcSample->data, (size_t)s.len);

    {
        int16 *pcm = (int16 *)s.data, n = s.len / 2, leftGain = amp, rightGain = amp;
        if (s.channels >= 2)
        {
            leftGain = (amp * (100 - MIN(100, MAX(0, balance)))) / 50;
            rightGain = (amp * MIN(100, MAX(0, balance))) / 50;
        }
        else
            leftGain = rightGain = amp;
        for (i = 0; i < n; i++)
        {
            int channel = s.channels > 1 ? (i % s.channels) : 0;
            int32 v = pcm[i];
            int32 gain = (channel == 0 ? leftGain : rightGain);
            v = (v * gain) / MAXAMPLITUDE;
            if (v > 32767)
                v = 32767;
            if (v < -32768)
                v = -32768;
            pcm[i] = (int16)v;
        }
    }
    memset(&src, 0, sizeof(src));
    src.format = SDL_AUDIO_S16LE;
    src.channels = s.channels;
    src.freq = freqOverride ? (int)((double)s.freq * (double)freqOverride / 32768.0 + 0.5) : s.freq;
    gVoices[sel].stream = SDL_CreateAudioStream(&src, NULL);
    if (!gVoices[sel].stream)
    {
        freesample(&s);
        return -1;
    }
    if (!SDL_BindAudioStream(gAudioDevice, gVoices[sel].stream))
    {
        stop_voice(&gVoices[sel]);
        freesample(&s);
        return -1;
    }
    if (!SDL_PutAudioStreamData(gVoices[sel].stream, s.data, s.len))
    {
        fprintf(stderr, "SDL3 audio: queue failed for SFX %d: %s\n", id, SDL_GetError());
        stop_voice(&gVoices[sel]);
        freesample(&s);
        return -1;
    }

    if (!SDL_FlushAudioStream(gVoices[sel].stream))
    {
        fprintf(stderr, "SDL3 audio: flush failed for SFX %d: %s\n", id, SDL_GetError());
        stop_voice(&gVoices[sel]);
        freesample(&s);
        return -1;
    }
    gVoices[sel].sample = s;
    gVoices[sel].soundID = id;
    gVoices[sel].active = 1;
    return 0;
}

static int start_music(const char *file, int reps, int amp)
{
    SDL_AudioSpec src;
    stop_voice(&gMusic);
    if (!gAudioReady)
        return -1;

    if (load_aifc(file, &gMusic.sample) < 0)
    {
        fprintf(stderr, "SDL3 audio: music load failed: %s\n", file ? file : "(null)");
        return -1;
    }

    memset(&src, 0, sizeof(src));
    src.format = SDL_AUDIO_S16LE;
    src.channels = gMusic.sample.channels;
    src.freq = gMusic.sample.freq;
    gMusic.stream = SDL_CreateAudioStream(&src, NULL);
    if (!gMusic.stream)
    {
        fprintf(stderr, "SDL3 audio: music stream creation failed for %s: %s\n", file, SDL_GetError());
        freesample(&gMusic.sample);
        return -1;
    }
    if (!SDL_BindAudioStream(gAudioDevice, gMusic.stream))
    {
        fprintf(stderr, "SDL3 audio: music stream bind failed for %s: %s\n", file, SDL_GetError());
        stop_voice(&gMusic);
        return -1;
    }
    gMusic.active = 1;
    gMusic.soundID = -1;
    gMusicLoop = reps;
    SDL_SetAudioStreamGain(gMusic.stream, (float)amp / (float)MAXAMPLITUDE);
    if (!SDL_PutAudioStreamData(gMusic.stream, gMusic.sample.data, gMusic.sample.len))
    {
        fprintf(stderr, "SDL3 audio: initial music queue failed for %s: %s\n", file, SDL_GetError());
        stop_voice(&gMusic);
        gMusicLoop = 0;
        return -1;
    }
    fprintf(stderr, "SDL3 audio: music started: %s (%d Hz, %d ch, %d bytes, reps=%d)\n", file, gMusic.sample.freq, gMusic.sample.channels, gMusic.sample.len, reps);
    return 0;
}

void SDL3_3DO_AudioPump(void)
{
    int queued;
    int i;

    for (i = 0; i < MAX_SDL_VOICES; i++)
    {
        if (gVoices[i].active && gVoices[i].stream)
        {
            if (SDL_GetAudioStreamQueued(gVoices[i].stream) == 0 &&
                SDL_GetAudioStreamAvailable(gVoices[i].stream) == 0)
                stop_voice(&gVoices[i]);
        }
    }

    if (!gMusic.active || !gMusic.stream || !gMusicLoop)
        return;
    queued = SDL_GetAudioStreamQueued(gMusic.stream);
    if (gMusicLoop == 1 && queued == 0 && SDL_GetAudioStreamAvailable(gMusic.stream) == 0)
    {
        stop_voice(&gMusic);
        gMusicLoop = 0;
        return;
    }
    if (queued < gMusic.sample.len / 2)
    {
        if (gMusicLoop > 1 || gMusicLoop < 0)
        {
            if (gMusicLoop > 1)
                gMusicLoop--;
            if (!SDL_PutAudioStreamData(gMusic.stream, gMusic.sample.data, gMusic.sample.len))
                fprintf(stderr, "SDL3 audio: music loop queue failed: %s\n", SDL_GetError());
        }
    }
}

int32 CallSound(soundPtr)
union CallSoundRec *soundPtr;
{
    int i, id;
    if (!soundPtr)
        return -1;
    switch (soundPtr->whatIWant)
    {
    case kInitializeSound:
    {
        SDL_AudioSpec want;
        memset(&want, 0, sizeof(want));
        want.format = SDL_AUDIO_S16LE;
        want.channels = 2;
        want.freq = 44100;
        gAudioDevice = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want);
        if (!gAudioDevice)
        {
            fprintf(stderr, "SDL3 audio init failed: %s\n", SDL_GetError());
            gAudioReady = 0;
            return -1;
        }
        gAudioReady = 1;

        SDL_ResumeAudioDevice(gAudioDevice);
        fprintf(stderr, "SDL3 audio: playback device opened (id=%u, requested 44100 Hz stereo S16LE)\n", (unsigned)gAudioDevice);
        break;
    }
    case kCleanupSound:
        SDL3_3DO_StopMovieAudio();
        stop_voice(&gMusic);
        for (i = 0; i < MAX_SDL_VOICES; i++)
            stop_voice(&gVoices[i]);
        for (i = 0; i < MAX_SDL_SOUNDS; i++)
            freesample(&gSounds[i]);
        if (gAudioDevice)
            SDL_CloseAudioDevice(gAudioDevice);
        gAudioDevice = 0;
        gAudioReady = 0;
        break;
    case kLoadRAMSound:
        id = soundPtr->loadSound.soundID;
        if (id < 0 || id >= MAX_SDL_SOUNDS)
            return -1;
        freesample(&gSounds[id]);
        return load_aifc(soundPtr->loadSound.soundFileName, &gSounds[id]);
    case kUnloadRAMSound:
        id = soundPtr->ramSound.soundID;
        if (id >= 0 && id < MAX_SDL_SOUNDS)
            freesample(&gSounds[id]);
        for (i = 0; i < MAX_SDL_VOICES; i++)
            if (gVoices[i].active && gVoices[i].soundID == id)
                stop_voice(&gVoices[i]);
        break;
    case kStartRAMSound:
        id = soundPtr->ramSound.soundID;
        if (id < 0 || id >= MAX_SDL_SOUNDS || !gSounds[id].data)
            return -1;
        return make_voice_from_sample(id, &gSounds[id], MAXAMPLITUDE, 50, 0);
    case kStopRAMSound:
        id = soundPtr->ramSound.soundID;
        for (i = 0; i < MAX_SDL_VOICES; i++)
            if (gVoices[i].active && gVoices[i].soundID == id)
                stop_voice(&gVoices[i]);
        break;
    case kSpoolSound:
        return start_music(soundPtr->spoolSound.fileToSpool, soundPtr->spoolSound.numReps, soundPtr->spoolSound.amplitude);
    case kStopSpoolingSound:
    case kStopFadeSpoolSound:
        stop_voice(&gMusic);
        gMusicLoop = 0;
        break;
    case kIsSoundSpooling:
        return gMusic.active;
    case kBeQuiet:
        if (gAudioDevice)
            SDL_SetAudioDeviceGain(gAudioDevice, 0.0f);
        break;
    case kBeNoisy:
        if (gAudioDevice)
            SDL_SetAudioDeviceGain(gAudioDevice, 1.0f);
        break;
    case kSetRAMSoundAmpl:
        break;
    case kSetRAMSoundFreq:
        break;
    default:
        break;
    }
    return 0;
}
