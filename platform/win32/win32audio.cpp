
#ifndef USE_OPENAL

#include "platform/i_sound.h"
#include "misc/error.h"
#include <Windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <thread>
#include <mutex>
#include <algorithm>

#undef min
#undef max

namespace
{
	IMMDevice *mmdevice;
	IAudioClient *audio_client;
	IAudioRenderClient *audio_render_client;
	HANDLE audio_buffer_ready_event = INVALID_HANDLE_VALUE;
	bool is_playing;
	UINT32 fragment_size;
	int wait_timeout;
	float* next_fragment;

	int mixing_frequency = 48000;
	int mixing_latency = 50;

	std::thread mixer_thread;
	std::mutex mixer_mutex;
	bool mixer_stop_flag;

	struct SoundSource
	{
		bool playing = false;
		int pos = 0;
		uint32_t frac = 0;
		const unsigned char* data = nullptr;
		int length = 0;
		int sampleRate = 0;
		float angle_x = 0.0f;
		float angle_y = 0.0f;
		float volume = 0.0f;
		bool loop = false;
	} sources[_MAX_VOICES];

	struct MusicSource
	{
		bool playing = false;
		int pos = 0;
		uint32_t frac = 0;
		int sample_rate = 0;
		std::vector<float> song_data;
		bool loop = false;
	} music;

	void mix_fragment()
	{
		std::unique_lock<std::mutex> lock(mixer_mutex);

		int count = fragment_size;
		float* output = next_fragment;
		for (int i = 0; i < count; i++)
		{
			output[0] = 0.0f;
			output[1] = 0.0f;
			output += 2;
		}

		for (int handle = 0; handle < _MAX_VOICES; handle++)
		{
			SoundSource& src = sources[handle];
			if (src.playing)
			{
				output = next_fragment;
				uint32_t speed = static_cast<uint32_t>(src.sampleRate * static_cast<uint64_t>(1 << 16) / mixing_frequency);
				int pos = src.pos;
				uint32_t frac = src.frac;
				int length = src.length;
				const unsigned char* data = src.data;
				float volume_left = src.volume * (1.0f + src.angle_x) * 0.5f;
				float volume_right = src.volume * (1.0f - src.angle_x) * 0.5f;
				for (int i = 0; i < count; i++)
				{
					float sample = (static_cast<int>(data[pos]) - 127) * (1.0f/127.0f);
					sample = std::max(sample, -1.0f);
					sample = std::min(sample, 1.0f);

					output[0] += sample * volume_left;
					output[1] += sample * volume_right;
					output += 2;

					frac += speed;
					pos += frac >> 16;
					frac &= 0xffff;
					if (pos >= length)
					{
						pos = pos % length;
						if (!src.loop)
						{
							pos = 0;
							frac = 0;
							src.playing = false;
							break;
						}
					}
				}
				src.pos = pos;
				src.frac = frac;
			}
		}

		if (music.playing)
		{
			output = next_fragment;
			uint32_t speed = static_cast<uint32_t>(music.sample_rate * static_cast<uint64_t>(1 << 16) / mixing_frequency);
			int pos = music.pos;
			uint32_t frac = music.frac;
			int length = music.song_data.size() / 2;
			const float* data = music.song_data.data();
			for (int i = 0; i < count; i++)
			{
				float sample = (static_cast<int>(data[pos]) - 127) * (1.0f / 127.0f);
				sample = std::max(sample, -1.0f);
				sample = std::min(sample, 1.0f);

				output[0] += data[pos << 1];
				output[1] += data[(pos << 1) + 1];
				output += 2;

				frac += speed;
				pos += frac >> 16;
				frac &= 0xffff;
				if (pos >= length)
				{
					pos = pos % length;
					if (!music.loop)
					{
						pos = 0;
						frac = 0;
						music.playing = false;
						break;
					}
				}
			}
			music.pos = pos;
			music.frac = frac;
		}
	}

	void write_fragment()
	{
		UINT32 write_pos = 0;
		while (write_pos < fragment_size)
		{
			WaitForSingleObject(audio_buffer_ready_event, wait_timeout);

			UINT32 num_padding_frames = 0;
			audio_client->GetCurrentPadding(&num_padding_frames);

			UINT32 buffer_available = fragment_size - num_padding_frames;
			UINT32 buffer_needed = fragment_size - write_pos;

			if (buffer_available < buffer_needed)
				ResetEvent(audio_buffer_ready_event);

			UINT32 buffer_size = std::min(buffer_needed, buffer_available);
			if (buffer_size > 0)
			{
				BYTE* buffer = 0;
				HRESULT result = audio_render_client->GetBuffer(buffer_size, &buffer);
				if (SUCCEEDED(result))
				{
					memcpy(buffer, next_fragment + write_pos * 2, sizeof(float) * 2 * buffer_size);
					result = audio_render_client->ReleaseBuffer(buffer_size, 0);

					if (!is_playing)
					{
						result = audio_client->Start();
						if (SUCCEEDED(result))
							is_playing = true;
					}
				}

				write_pos += buffer_size;
			}
		}
	}

	void mixer_thread_main()
	{
		std::unique_lock<std::mutex> lock(mixer_mutex);
		while (!mixer_stop_flag)
		{
			lock.unlock();
			mix_fragment();
			write_fragment();
			lock.lock();
		}
	}

	void start_mixer_thread()
	{
		std::unique_lock<std::mutex> lock(mixer_mutex);
		mixer_stop_flag = false;
		lock.unlock();
		mixer_thread = std::thread(mixer_thread_main);
	}

	void stop_mixer_thread()
	{
		std::unique_lock<std::mutex> lock(mixer_mutex);
		mixer_stop_flag = true;
		lock.unlock();
		mixer_thread.join();
	}
}

void I_ErrorCheck(char* context)
{
}

int I_InitAudio()
{
	CoInitialize(0);

	IMMDeviceEnumerator* device_enumerator = nullptr;
	HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), 0, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&device_enumerator);
	if (FAILED(result))
	{
		Error("Unable to create IMMDeviceEnumerator instance\n");
		return 1;
	}

	result = device_enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &mmdevice);
	device_enumerator->Release();
	if (FAILED(result))
	{
		Error("IDeviceEnumerator.GetDefaultAudioEndpoint failed\n");
		return 1;
	}

	result = mmdevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, 0, (void**)&audio_client);
	if (FAILED(result))
	{
		Error("IMMDevice.Activate failed\n");
		mmdevice->Release();
		return 1;
	}

	WAVEFORMATEXTENSIBLE wave_format;
	wave_format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	wave_format.Format.nChannels = 2;
	wave_format.Format.nBlockAlign = 2 * sizeof(float);
	wave_format.Format.wBitsPerSample = 8 * sizeof(float);
	wave_format.Format.cbSize = 22;
	wave_format.Samples.wValidBitsPerSample = wave_format.Format.wBitsPerSample;
	wave_format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
	wave_format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

	wave_format.Format.nSamplesPerSec = mixing_frequency;
	wave_format.Format.nAvgBytesPerSec = wave_format.Format.nSamplesPerSec * wave_format.Format.nBlockAlign;

	WAVEFORMATEX* closest_match = 0;
	result = audio_client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, (WAVEFORMATEX*)& wave_format, &closest_match);
	if (FAILED(result))
	{
		Error("IAudioClient.IsFormatSupported failed\n");
		audio_client->Release();
		mmdevice->Release();
		audio_client = nullptr;
		mmdevice = nullptr;
		return 1;
	}

	// We could not get the exact format we wanted. Try to use the frequency that the closest matching format is using:
	if (result == S_FALSE)
	{
		mixing_frequency = closest_match->nSamplesPerSec;
		wait_timeout = mixing_latency * 2;
		wave_format.Format.nSamplesPerSec = mixing_frequency;
		wave_format.Format.nAvgBytesPerSec = wave_format.Format.nSamplesPerSec * wave_format.Format.nBlockAlign;

		CoTaskMemFree(closest_match);
		closest_match = 0;
	}

	result = audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, mixing_latency * (REFERENCE_TIME)1000, 0, (WAVEFORMATEX*)& wave_format, 0);
	if (FAILED(result))
	{
		Error("IAudioClient.Initialize failed\n");
		audio_client->Release();
		mmdevice->Release();
		audio_client = nullptr;
		mmdevice = nullptr;
		return 1;
	}

	result = audio_client->GetService(__uuidof(IAudioRenderClient), (void**)&audio_render_client);
	if (FAILED(result))
	{
		Error("IAudioClient.GetService(IAudioRenderClient) failed\n");
		audio_client->Release();
		mmdevice->Release();
		audio_client = nullptr;
		mmdevice = nullptr;
		return 1;
	}

	audio_buffer_ready_event = CreateEvent(0, TRUE, TRUE, 0);
	if (audio_buffer_ready_event == INVALID_HANDLE_VALUE)
	{
		Error("CreateEvent failed\n");
		audio_render_client->Release();
		audio_client->Release();
		mmdevice->Release();
		audio_render_client = nullptr;
		audio_client = nullptr;
		mmdevice = nullptr;
		return 1;
	}

	result = audio_client->SetEventHandle(audio_buffer_ready_event);
	if (FAILED(result))
	{
		Error("IAudioClient.SetEventHandle failed\n");
		audio_render_client->Release();
		audio_client->Release();
		mmdevice->Release();
		audio_render_client = nullptr;
		audio_client = nullptr;
		mmdevice = nullptr;
		return 1;
	}

	result = audio_client->GetBufferSize(&fragment_size);
	if (FAILED(result))
	{
		Error("IAudioClient.GetBufferSize failed\n");
		audio_render_client->Release();
		audio_client->Release();
		mmdevice->Release();
		audio_render_client = nullptr;
		audio_client = nullptr;
		mmdevice = nullptr;
		return 1;
	}

	next_fragment = new float[2 * fragment_size];
	start_mixer_thread();

	return 0;
}

void I_ShutdownAudio()
{
	if (audio_render_client)
	{
		stop_mixer_thread();
		if (is_playing)
			audio_client->Stop();
		audio_render_client->Release();
		audio_client->Release();
		mmdevice->Release();
		CloseHandle(audio_buffer_ready_event);
		delete[] next_fragment;
		audio_render_client = nullptr;
		audio_client = nullptr;
		mmdevice = nullptr;
		audio_buffer_ready_event = INVALID_HANDLE_VALUE;
		next_fragment = nullptr;
	}
}

int I_GetSoundHandle()
{
	std::unique_lock<std::mutex> lock(mixer_mutex);
	for (int i = 0; i < _MAX_VOICES; i++)
	{
		if (!sources[i].playing)
			return i;
	}
	return _ERR_NO_SLOTS;
}

void I_SetSoundData(int handle, unsigned char* data, int length, int sampleRate)
{
	if (handle < 0 || handle >= _MAX_VOICES) return;

	std::unique_lock<std::mutex> lock(mixer_mutex);
	sources[handle].data = data;
	sources[handle].length = length;
	sources[handle].sampleRate = sampleRate;
	sources[handle].pos = 0;
	sources[handle].frac = 0;
	sources[handle].playing = false;
	sources[handle].loop = false;
}

void I_SetSoundInformation(int handle, int volume, int angle)
{
	if (handle < 0 || handle >= _MAX_VOICES) return;

	float flang = (angle / 65536.0f) * (3.1415927f * 2);
	float x = (float)cos(flang) * .05f;
	float y = (float)sin(flang) * .05f;

	std::unique_lock<std::mutex> lock(mixer_mutex);
	sources[handle].angle_x = x;
	sources[handle].angle_y = y;
	sources[handle].volume = volume / 65536.0f;
}

void I_SetAngle(int handle, int angle)
{
	if (handle < 0 ||handle >= _MAX_VOICES) return;

	float flang = (angle / 65536.0f) * (3.1415927f * 2);
	float x = (float)cos(flang) * .05f;
	float y = (float)sin(flang) * .05f;

	std::unique_lock<std::mutex> lock(mixer_mutex);
	sources[handle].angle_x = x;
	sources[handle].angle_y = y;
}

void I_SetVolume(int handle, int volume)
{
	if (handle < 0 || handle >= _MAX_VOICES) return;

	std::unique_lock<std::mutex> lock(mixer_mutex);
	sources[handle].volume = volume / 65536.0f;
}

void I_PlaySound(int handle, int loop)
{
	if (handle < 0 || handle >= _MAX_VOICES) return;

	std::unique_lock<std::mutex> lock(mixer_mutex);
	sources[handle].pos = 0;
	sources[handle].frac = 0;
	sources[handle].playing = true;
	sources[handle].loop = loop;
}

void I_StopSound(int handle)
{
	if (handle < 0 || handle >= _MAX_VOICES) return;

	std::unique_lock<std::mutex> lock(mixer_mutex);
	sources[handle].playing = false;
}

int I_CheckSoundPlaying(int handle)
{
	if (handle < 0 || handle >= _MAX_VOICES) return 0;

	std::unique_lock<std::mutex> lock(mixer_mutex);
	return sources[handle].playing;
}

int I_CheckSoundDone(int handle)
{
	return !I_CheckSoundPlaying(handle);
}

void I_PlayHQSong(int sample_rate, std::vector<float>&& song_data, bool loop)
{
	std::unique_lock<std::mutex> lock(mixer_mutex);
	music.sample_rate = sample_rate;
	music.song_data = std::move(song_data);
	music.loop = loop;
	music.pos = 0;
	music.frac = 0;
	music.playing = true;
}

void I_StopHQSong()
{
	std::unique_lock<std::mutex> lock(mixer_mutex);
	music.sample_rate = 0;
	music.song_data.clear();
	music.loop = false;
	music.pos = 0;
	music.frac = 0;
	music.playing = false;
}

#endif
