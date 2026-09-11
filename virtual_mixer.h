#ifndef VIRTUAL_MIXER_H
#define VIRTUAL_MIXER_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_MIXER_CHANNELS 64
#define MIXER_SAMPLE_RATE 44100
#define MIXER_BUFFER_SIZE 4096

typedef struct {
    int16_t* buffer;           // Audio buffer for this channel
    size_t buffer_size;        // Total buffer size
    size_t write_pos;          // Current write position
    size_t read_pos;           // Current read position
    bool active;               // Is this channel in use
    float volume;              // Channel volume (0.0 to 1.0)
    float pan;                 // Channel pan (-1.0 to 1.0)
} MixerChannel;

typedef struct {
    MixerChannel channels[MAX_MIXER_CHANNELS];
    int16_t* output_buffer;    // Final mixed output buffer
    size_t output_buffer_size;
    
    // Mixer configuration
    int sample_rate;
    int num_channels;
    bool normalize;
} VirtualMixer;

// Initialize the virtual mixer
VirtualMixer* mixer_init(int sample_rate, int num_channels, bool normalize);

// Free the mixer resources
void mixer_free(VirtualMixer* mixer);

// Allocate a new mixer channel
int mixer_allocate_channel(VirtualMixer* mixer);

// Release a mixer channel
void mixer_release_channel(VirtualMixer* mixer, int channel_id);

// Write audio data to a mixer channel
void mixer_write_channel(VirtualMixer* mixer, int channel_id, 
                         const int16_t* data, size_t size);

// Mix all active channels
size_t mixer_mix_channels(VirtualMixer* mixer);

// Get the mixed output buffer
int16_t* mixer_get_output(VirtualMixer* mixer, size_t* out_size);

// Set channel volume and pan
void mixer_set_channel_volume(VirtualMixer* mixer, int channel_id, 
                              float volume, float pan);

#endif // VIRTUAL_MIXER_H
