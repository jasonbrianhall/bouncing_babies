#ifndef DBOPL_WRAPPER_H
#define DBOPL_WRAPPER_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_OPL_CHANNELS 36

// OPL channel structure for tracking state
typedef struct {
    bool active;
    int midi_channel;
    int midi_note;
    int instrument;
    int velocity;
    int pan;
    uint32_t start_time;  // For note age tracking
} OPLChannel;

// Initialize the OPL emulator
void OPL_Init(int sample_rate);

// Reset the OPL emulator
void OPL_Reset(void);

// Write to OPL register
void OPL_WriteReg(uint32_t reg, uint8_t value);

// Generate audio samples
void OPL_Generate(int16_t *buffer, int num_samples);

// Clean up OPL resources
void OPL_Shutdown(void);

// Helper functions for MIDI player
void OPL_NoteOn(int channel, int note, int velocity);
void OPL_NoteOff(int channel, int note);
void OPL_ProgramChange(int channel, int program);
void OPL_SetPan(int channel, int pan);
void OPL_SetVolume(int channel, int volume);
void OPL_SetPitchBend(int channel, int bend);
void set_channel_volume(int opl_channel, int velocity, int volume);

// Expose OPL channels array for advanced MIDI control
extern OPLChannel opl_channels[MAX_OPL_CHANNELS];

// Load instrument data from your existing instruments.c
extern void OPL_LoadInstruments(void);

#endif // DBOPL_WRAPPER_H
