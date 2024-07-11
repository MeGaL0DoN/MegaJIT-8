#define MINIAUDIO_IMPLEMENTATION
#include <MiniAudio/miniaudio.h>

#include "ChipCore.h"

extern ChipState s;

static ma_device soundDevice;
static ma_waveform waveForm;

void sound_data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
	if (ChipCore::enableAudio && s.sound_timer > 0)
		ma_waveform_read_pcm_frames(&waveForm, pOutput, frameCount, nullptr);
}

void ChipCore::initAudio()
{
	enableAudio = true;

	constexpr double initialVolume = 0.5;
	constexpr int frequency = 440;

	ma_waveform_config config;
	ma_device_config deviceConfig;

	config = ma_waveform_config_init(ma_format_f32, 2, 44100, ma_waveform_type_square, initialVolume, frequency);
	ma_waveform_init(&config, &waveForm);

	deviceConfig = ma_device_config_init(ma_device_type_playback);
	deviceConfig.playback.format = ma_format_f32;
	deviceConfig.playback.channels = 2;
	deviceConfig.sampleRate = 44100;
	deviceConfig.dataCallback = sound_data_callback;

	ma_device_init(NULL, &deviceConfig, &soundDevice);
	ma_device_start(&soundDevice);
}

void ChipCore::setVolume(double val)
{
	ma_waveform_set_amplitude(&waveForm, val);
}