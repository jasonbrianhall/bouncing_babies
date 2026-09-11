#include "midi_render.h"
#include "midiplayer.h"
#include "dbopl_wrapper.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

// ============================================================================
// Global MIDI player state
//
// Ported from midiplayer.cpp, minus the SDL audio device / console / mixer
// pieces we don't need for offline rendering. This state is global (as in
// the original single-instance player), so render_midi_to_wav() serializes
// on g_midi_render_mutex — only one render runs at a time.
// ============================================================================

struct FMInstrument adl[181];
int globalVolume = 100;
bool enableNormalization = true;
bool isPlaying = false;
bool paused = false;

FILE* midiFile = nullptr;
int TrackCount = 0;
int DeltaTicks = 0;
double Tempo = 500000;  // Default 120 BPM
double playTime = 0;

int tkPtr[MAX_TRACKS];
double tkDelay[MAX_TRACKS];
int tkStatus[MAX_TRACKS];
bool loopStart = false;
bool loopEnd = false;
int loPtr[MAX_TRACKS];
double loDelay[MAX_TRACKS];
int loStatus[MAX_TRACKS];
double loopwait = 0;
int rbPtr[MAX_TRACKS];
double rbDelay[MAX_TRACKS];
int rbStatus[MAX_TRACKS];
double playwait = 0;

int ChPatch[16];
double ChBend[16];
int ChVolume[16];
int ChPanning[16];
int ChVibrato[16];

// Declared for linkage with midiplayer.h / dbopl_wrapper.cpp; unused here
// since offline rendering bypasses the live mixer entirely.
VirtualMixer* g_midi_mixer = nullptr;
int g_midi_mixer_channel = -1;

static std::mutex g_midi_render_mutex;

// ============================================================================
// MIDI file parsing (ported unchanged from midiplayer.cpp)
// ============================================================================

unsigned long readVarLen(FILE* f) {
    unsigned char c;
    unsigned long value = 0;

    if (fread(&c, 1, 1, f) != 1) return 0;

    value = c;
    if (c & 0x80) {
        value &= 0x7F;
        do {
            if (fread(&c, 1, 1, f) != 1) return value;
            value = (value << 7) + (c & 0x7F);
        } while (c & 0x80);
    }

    return value;
}

int readString(FILE* f, int len, char* str) {
    return fread(str, 1, len, f);
}

unsigned long convertInteger(char* str, int len) {
    unsigned long value = 0;
    for (int i = 0; i < len; i++) {
        value = value * 256 + (unsigned char)str[i];
    }
    return value;
}

bool loadMidiFile(const char* filename) {
    char buffer[256];
    char id[5] = {0};
    unsigned long headerLength;
    int format;

    midiFile = fopen(filename, "rb");
    if (!midiFile) {
        return false;
    }

    if (readString(midiFile, 4, id) != 4 || strncmp(id, "MThd", 4) != 0) {
        fclose(midiFile);
        midiFile = nullptr;
        return false;
    }

    readString(midiFile, 4, buffer);
    headerLength = convertInteger(buffer, 4);
    if (headerLength != 6) {
        fclose(midiFile);
        midiFile = nullptr;
        return false;
    }

    readString(midiFile, 2, buffer);
    format = (int)convertInteger(buffer, 2);
    (void)format;

    readString(midiFile, 2, buffer);
    TrackCount = (int)convertInteger(buffer, 2);
    if (TrackCount > MAX_TRACKS) {
        fclose(midiFile);
        midiFile = nullptr;
        return false;
    }

    readString(midiFile, 2, buffer);
    DeltaTicks = (int)convertInteger(buffer, 2);

    for (int tk = 0; tk < TrackCount; tk++) {
        if (readString(midiFile, 4, id) != 4 || strncmp(id, "MTrk", 4) != 0) {
            fclose(midiFile);
            midiFile = nullptr;
            return false;
        }

        readString(midiFile, 4, buffer);
        unsigned long trackLength = convertInteger(buffer, 4);
        long pos = ftell(midiFile);

        tkDelay[tk] = readVarLen(midiFile);
        tkPtr[tk] = ftell(midiFile);

        fseek(midiFile, pos + (long)trackLength, SEEK_SET);
    }

    fseek(midiFile, 0, SEEK_SET);
    return true;
}

void handleMidiEvent(int tk) {
    unsigned char status, data1, data2;
    unsigned char evtype;
    unsigned long len;

    fseek(midiFile, tkPtr[tk], SEEK_SET);

    if (fread(&status, 1, 1, midiFile) != 1) return;

    if (status < 0x80) {
        fseek(midiFile, tkPtr[tk], SEEK_SET);
        status = tkStatus[tk];
    } else {
        tkStatus[tk] = status;
    }

    int midCh = status & 0x0F;

    switch (status & 0xF0) {
        case NOTE_OFF: {
            fread(&data1, 1, 1, midiFile);
            fread(&data2, 1, 1, midiFile);

            ChBend[midCh] = 0;
            OPL_NoteOff(midCh, data1);
            break;
        }

        case NOTE_ON: {
            fread(&data1, 1, 1, midiFile);
            fread(&data2, 1, 1, midiFile);

            if (data2 == 0) {
                ChBend[midCh] = 0;
                OPL_NoteOff(midCh, data1);
                break;
            }

            OPL_NoteOn(midCh, data1, data2);
            break;
        }

        case CONTROL_CHANGE: {
            fread(&data1, 1, 1, midiFile);
            fread(&data2, 1, 1, midiFile);

            switch (data1) {
                case 1:
                    ChVibrato[midCh] = data2;
                    break;

                case 7:
                    ChVolume[midCh] = data2;
                    OPL_SetVolume(midCh, data2);
                    break;

                case 10:
                    ChPanning[midCh] = data2;
                    OPL_SetPan(midCh, data2);
                    break;

                case 11:
                    for (int i = 0; i < MAX_OPL_CHANNELS; i++) {
                        if (opl_channels[i].active && opl_channels[i].midi_channel == midCh) {
                            set_channel_volume(i, opl_channels[i].velocity,
                                             (ChVolume[midCh] * data2) / 127);
                        }
                    }
                    break;

                case 120:
                    OPL_Reset();
                    break;

                case 121:
                    for (int i = 0; i < 16; i++) {
                        ChBend[i] = 0;
                        ChVibrato[i] = 0;
                    }
                    break;

                case 123:
                    for (int i = 0; i < MAX_OPL_CHANNELS; i++) {
                        if (opl_channels[i].active && opl_channels[i].midi_channel == midCh) {
                            OPL_NoteOff(midCh, opl_channels[i].midi_note);
                        }
                    }
                    break;
            }
            break;
        }

        case PROGRAM_CHANGE: {
            fread(&data1, 1, 1, midiFile);
            ChPatch[midCh] = data1;
            OPL_ProgramChange(midCh, data1);
            break;
        }

        case CHAN_PRESSURE: {
            fread(&data1, 1, 1, midiFile);
            for (int i = 0; i < MAX_OPL_CHANNELS; i++) {
                if (opl_channels[i].active && opl_channels[i].midi_channel == midCh) {
                    set_channel_volume(i, opl_channels[i].velocity,
                                     (ChVolume[midCh] * data1) / 127);
                }
            }
            break;
        }

        case PITCH_BEND: {
            fread(&data1, 1, 1, midiFile);
            fread(&data2, 1, 1, midiFile);

            int bend = (data2 << 7) | data1;
            ChBend[midCh] = bend;

            OPL_SetPitchBend(midCh, bend);
            break;
        }

        case META_EVENT: case SYSTEM_MESSAGE: {
            if (status == META_EVENT) {
                fread(&evtype, 1, 1, midiFile);
                len = readVarLen(midiFile);

                if (evtype == META_END_OF_TRACK) {
                    tkStatus[tk] = -1;
                    fseek(midiFile, len, SEEK_CUR);
                } else if (evtype == META_TEMPO) {
                    char tempo[4] = {0};
                    readString(midiFile, (int)len, tempo);
                    unsigned long tempoVal = convertInteger(tempo, (int)len);
                    Tempo = tempoVal;
                } else if (evtype == META_TEXT) {
                    char text[256] = {0};
                    readString(midiFile, (int)len < 255 ? (int)len : 255, text);

                    if (strcmp(text, "loopStart") == 0) {
                        loopStart = true;
                    } else if (strcmp(text, "loopEnd") == 0) {
                        loopEnd = true;
                    } else if (strstr(text, "volume=") == text) {
                        int volume = atoi(text + 7);
                        if (volume >= 0 && volume <= 127) {
                            ChVolume[midCh] = volume;
                            OPL_SetVolume(midCh, volume);
                        }
                    } else if (strstr(text, "instrument=") == text) {
                        int instrument = atoi(text + 11);
                        if (instrument >= 0 && instrument < 181) {
                            ChPatch[midCh] = instrument;
                            OPL_ProgramChange(midCh, instrument);
                        }
                    }
                } else {
                    fseek(midiFile, len, SEEK_CUR);
                }
            } else {
                len = readVarLen(midiFile);
                fseek(midiFile, (long)len, SEEK_CUR);
            }
            break;
        }

        default: {
            switch (status & 0xF0) {
                case 0xC0:
                case 0xD0:
                    fseek(midiFile, 1, SEEK_CUR);
                    break;
                default:
                    fseek(midiFile, 2, SEEK_CUR);
                    break;
            }
            break;
        }
    }

    unsigned long nextDelay = readVarLen(midiFile);
    tkDelay[tk] += nextDelay;

    tkPtr[tk] = ftell(midiFile);
}

void processEvents() {
    for (int tk = 0; tk < TrackCount; tk++) {
        rbPtr[tk] = tkPtr[tk];
        rbDelay[tk] = tkDelay[tk];
        rbStatus[tk] = tkStatus[tk];

        if (tkStatus[tk] >= 0 && tkDelay[tk] <= 0) {
            handleMidiEvent(tk);
        }
    }

    if (loopStart) {
        for (int tk = 0; tk < TrackCount; tk++) {
            loPtr[tk] = rbPtr[tk];
            loDelay[tk] = rbDelay[tk];
            loStatus[tk] = rbStatus[tk];
        }
        loopwait = playwait;
        loopStart = false;
    } else if (loopEnd) {
        for (int tk = 0; tk < TrackCount; tk++) {
            tkPtr[tk] = loPtr[tk];
            tkDelay[tk] = loDelay[tk];
            tkStatus[tk] = loStatus[tk];
        }
        loopEnd = false;
        playwait = loopwait;
    }

    double nextDelay = -1;
    for (int tk = 0; tk < TrackCount; tk++) {
        if (tkStatus[tk] < 0) continue;
        if (nextDelay == -1 || tkDelay[tk] < nextDelay) {
            nextDelay = tkDelay[tk];
        }
    }

    bool allEnded = true;
    for (int tk = 0; tk < TrackCount; tk++) {
        if (tkStatus[tk] >= 0) {
            allEnded = false;
            break;
        }
    }

    if (allEnded) {
        if (loopwait > 0) {
            for (int tk = 0; tk < TrackCount; tk++) {
                tkPtr[tk] = loPtr[tk];
                tkDelay[tk] = loDelay[tk];
                tkStatus[tk] = loStatus[tk];
            }
            playwait = loopwait;
        } else {
            isPlaying = false;
            return;
        }
    }

    for (int tk = 0; tk < TrackCount; tk++) {
        tkDelay[tk] -= nextDelay;
    }

    double t = nextDelay * Tempo / (DeltaTicks * 1000000.0);
    playwait += t;
}

// ============================================================================
// Offline render: MIDI bytes in, WAV bytes out. No files exposed to the
// caller — the only file I/O is a private temp file for the MIDI parser's
// fopen()-based reader, deleted immediately after loading.
// ============================================================================

static std::string make_temp_midi_path() {
#ifdef _WIN32
    char tmp_dir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp_dir);
    char tmp_file[MAX_PATH];
    GetTempFileNameA(tmp_dir, "mid", 0, tmp_file);
    return std::string(tmp_file);
#else
    char tmpl[] = "/tmp/midi_render_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0) close(fd);
    return std::string(tmpl);
#endif
}

bool render_midi_to_wav(const std::vector<uint8_t> &midi_bytes,
                         std::vector<uint8_t> &out_wav,
                         int volume) {
    std::lock_guard<std::mutex> lock(g_midi_render_mutex);

    std::string tmp_path = make_temp_midi_path();
    FILE* tf = fopen(tmp_path.c_str(), "wb");
    if (!tf) return false;
    fwrite(midi_bytes.data(), 1, midi_bytes.size(), tf);
    fclose(tf);

    // Reset all playback state before this render
    memset(tkPtr, 0, sizeof(tkPtr));
    memset(tkDelay, 0, sizeof(tkDelay));
    memset(tkStatus, 0, sizeof(tkStatus));
    memset(loPtr, 0, sizeof(loPtr));
    memset(loDelay, 0, sizeof(loDelay));
    memset(loStatus, 0, sizeof(loStatus));
    memset(rbPtr, 0, sizeof(rbPtr));
    memset(rbDelay, 0, sizeof(rbDelay));
    memset(rbStatus, 0, sizeof(rbStatus));
    TrackCount = 0;
    DeltaTicks = 0;
    Tempo = 500000;
    playTime = 0;
    loopStart = false;
    loopEnd = false;
    playwait = 0;
    loopwait = 0;
    globalVolume = volume;
    for (int i = 0; i < 16; i++) {
        ChPatch[i] = 0;
        ChBend[i] = 0;
        ChVolume[i] = 127;
        ChPanning[i] = 64;
        ChVibrato[i] = 0;
    }

    OPL_Init(SAMPLE_RATE);
    OPL_LoadInstruments();
    OPL_Reset();

    bool ok = loadMidiFile(tmp_path.c_str());
    remove(tmp_path.c_str());

    if (!ok) {
        OPL_Shutdown();
        return false;
    }

    isPlaying = true;
    paused = false;

    std::vector<uint8_t> pcm;
    const int SR = SAMPLE_RATE;
    const int CH = AUDIO_CHANNELS;
    const int BUF = AUDIO_BUFFER;
    int16_t audio_buffer[BUF * CH];
    const double buffer_duration = (double)BUF / SR;
    const double MAX_SECONDS = 600.0;  // safety cap against malformed/looping files

    processEvents();
    while (isPlaying && playTime < MAX_SECONDS) {
        memset(audio_buffer, 0, sizeof(audio_buffer));
        OPL_Generate(audio_buffer, BUF);

        const uint8_t* p = reinterpret_cast<const uint8_t*>(audio_buffer);
        pcm.insert(pcm.end(), p, p + (size_t)BUF * CH * sizeof(int16_t));

        playTime += buffer_duration;
        playwait -= buffer_duration;
        while (playwait <= 0 && isPlaying) {
            processEvents();
        }
    }

    if (midiFile) {
        fclose(midiFile);
        midiFile = nullptr;
    }
    OPL_Shutdown();

    // Build the WAV byte stream: 44-byte PCM header + samples
    uint32_t data_size = (uint32_t)pcm.size();
    uint32_t byte_rate = SR * CH * 2;
    uint16_t block_align = (uint16_t)(CH * 2);
    uint32_t riff_size = 36 + data_size;

    out_wav.clear();
    out_wav.reserve(44 + pcm.size());

    auto push32 = [&](uint32_t v) {
        for (int i = 0; i < 4; i++) out_wav.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
    };
    auto push16 = [&](uint16_t v) {
        for (int i = 0; i < 2; i++) out_wav.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
    };
    auto push_tag = [&](const char* s) {
        out_wav.insert(out_wav.end(), s, s + 4);
    };

    push_tag("RIFF"); push32(riff_size); push_tag("WAVE");
    push_tag("fmt "); push32(16); push16(1); push16((uint16_t)CH);
    push32(SR); push32(byte_rate); push16(block_align); push16(16);
    push_tag("data"); push32(data_size);
    out_wav.insert(out_wav.end(), pcm.begin(), pcm.end());

    return true;
}
