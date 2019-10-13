/*
The code contained in this file is not the property of Parallax Software,
and is not under the terms of the Parallax Software Source license.
Instead, it is released under the terms of the MIT License.
*/

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <thread>
#include <mutex>

#ifdef USE_OPENAL

#include "AL/al.h"
#include "AL/alc.h"
#include "AL/alext.h"
#include "platform/i_sound.h"
#include "platform/s_midi.h"
#include "platform/s_sequencer.h"
#include "misc/error.h"
//#include "mem/mem.h" //[ISB] mem.h isn't thread safe so uh

ALCdevice *ALDevice = NULL;
ALCcontext *ALContext = NULL;

ALuint bufferNames[_MAX_VOICES];
ALuint sourceNames[_MAX_VOICES];

//MIDI audio fields
#define MAX_BUFFERS_QUEUED 5

hmpheader_t* CurrentSong;
bool StopMIDI = true;
bool LoopMusic;

std::mutex MIDIMutex;
std::thread MIDIThread;

ALuint BufferQueue[MAX_BUFFERS_QUEUED];
ALuint MusicSource;
int CurrentBuffers = 0;
ALushort* MusicBufferData;

//HQ audio fields
ALuint HQMusicSource;
ALuint HQMusicBuffer;
bool HQMusicPlaying = false;

int MusicVolume;

void I_ErrorCheck(const char* context)
{
	int error;
	error = alGetError();
	if (error != AL_NO_ERROR)
	{
		fprintf(stderr, "Error in context %s: ", context);
		if (error == AL_INVALID_ENUM)
			fprintf(stderr, "Invalid enum\n");
		else if (error == AL_INVALID_NAME)
			fprintf(stderr, "Invalid name\n");
		else if (error == AL_INVALID_OPERATION)
			fprintf(stderr, "Invalid operation\n");
		else if (error == AL_INVALID_VALUE)
			fprintf(stderr, "Invalid value\n");
	}
}

void I_CreateMusicSource();

int I_InitAudio()
{
	int i;
	ALDevice = alcOpenDevice(NULL);
	if (ALDevice == NULL)
	{
		Warning("I_InitAudio: Cannot open OpenAL device\n");
		return 1;
	}
	ALContext = alcCreateContext(ALDevice, 0);
	if (ALContext == NULL)
	{
		Warning("I_InitAudio: Cannot create OpenAL context\n");
		I_ShutdownAudio();
		return 1;
	}
	alcMakeContextCurrent(ALContext);

	alGenBuffers(_MAX_VOICES, &bufferNames[0]);
	I_ErrorCheck("Creating buffers");
	/*alGenSources(_MAX_VOICES, &sourceNames[0]);
	I_ErrorCheck("Creating sources");
	for (i = 0; i < _MAX_VOICES; i++)
	{
		alSourcef(sourceNames[i], AL_ROLLOFF_FACTOR, 0.0f);
	}*/
	float orientation[] = { 0.0, 0.0, -1.0, 0.0, 1.0, 0.0 };
	alListenerfv(AL_ORIENTATION, &orientation[0]);
	I_ErrorCheck("Listener hack");

	if (!alIsExtensionPresent("AL_EXT_FLOAT32"))
	{
		printf("oops no float still\n");
	}

	return 0;
}

void I_ShutdownAudio()
{
	if (ALDevice)
	{
		alcMakeContextCurrent(NULL);
		if (ALContext)
			alcDestroyContext(ALContext);
		alcCloseDevice(ALDevice);
	}
}

int I_GetSoundHandle()
{
	int i;
	ALint state;
	for (i = 0; i < _MAX_VOICES; i++)
	{
		if (alIsSource(sourceNames[i]))
		{
			alGetSourcei(sourceNames[i], AL_SOURCE_STATE, &state);
			if (state != AL_PLAYING)
			{
				alDeleteSources(1, &sourceNames[i]); //[ISB] delete the previous source before using this handle. Fixes problems with distant sounds briefly sounding loud
				alGenSources(1, &sourceNames[i]);
				return i;
			}
		}
		else
		{
			alGenSources(1, &sourceNames[i]);
			return i;
		}
	}
	I_ErrorCheck("Getting handle");
	return _ERR_NO_SLOTS;
}

void I_SetSoundData(int handle, unsigned char* data, int length, int sampleRate)
{
	if (handle >= _MAX_VOICES) return;

	alSourcei(sourceNames[handle], AL_BUFFER, NULL);
	alBufferData(bufferNames[handle], AL_FORMAT_MONO8, data, length, sampleRate);
	alSourcei(sourceNames[handle], AL_BUFFER, bufferNames[handle]);
	I_ErrorCheck("Setting sound data");
}

void I_SetSoundInformation(int handle, int volume, int angle)
{
	if (handle >= _MAX_VOICES) return;
	I_SetAngle(handle, angle);
	I_SetVolume(handle, volume);
}

void I_SetAngle(int handle, int angle)
{
	if (handle >= _MAX_VOICES) return;

	float x, y;
	float flang = (angle / 65536.0f) * (3.1415927f);

	x = (float)cos(flang);
	y = (float)sin(flang);

	alSource3f(sourceNames[handle], AL_POSITION, -x, 0.0f, y);
	I_ErrorCheck("Setting sound angle");
}

void I_SetVolume(int handle, int volume)
{
	if (handle >= _MAX_VOICES) return;

	float gain = volume / 65536.0f;
	alSourcef(sourceNames[handle], AL_GAIN, gain);
	I_ErrorCheck("Setting sound volume");
}

void I_PlaySound(int handle, int loop)
{
	if (handle >= _MAX_VOICES) return;
	alSourcei(sourceNames[handle], AL_LOOPING, loop);
	alSourcePlay(sourceNames[handle]);
	I_ErrorCheck("Playing sound");
}

void I_StopSound(int handle)
{
	if (handle >= _MAX_VOICES) return;
	alSourceStop(sourceNames[handle]);
	I_ErrorCheck("Stopping sound");
}

int I_CheckSoundPlaying(int handle)
{
	if (handle >= _MAX_VOICES) return 0;
	
	int playing;
	alGetSourcei(sourceNames[handle], AL_SOURCE_STATE, &playing);
	return playing == AL_PLAYING;
}

int I_CheckSoundDone(int handle)
{
	if (handle >= _MAX_VOICES) return 0;

	int playing;
	alGetSourcei(sourceNames[handle], AL_SOURCE_STATE, &playing);
	return playing == AL_STOPPED;
}

//-----------------------------------------------------------------------------
// Emitting pleasing rythmic sound at player
//-----------------------------------------------------------------------------
bool playing = false;

void I_SetMusicVolume(int volume)
{
	//printf("Music volume %d\n", volume);
	MusicVolume = volume;
	if (alIsSource(MusicSource)) //[ISB] TODO okay so this isn't truly thread safe, it likely won't pose a problem, but I should fix it just in case
	{
		alSourcef(MusicSource, AL_GAIN, MusicVolume / 127.0f);
	}
	if (alIsSource(HQMusicSource)) //[ISB] heh
	{
		alSourcef(HQMusicSource, AL_GAIN, MusicVolume / 127.0f);
	}
	I_ErrorCheck("Setting music volume");
}

void I_PlayHQSong(int sample_rate, std::vector<float>&& song_data, bool loop)
{
	alGenSources(1, &HQMusicSource);
	alSourcef(HQMusicSource, AL_ROLLOFF_FACTOR, 0.0f);
	alSource3f(HQMusicSource, AL_POSITION, 1.0f, 0.0f, 0.0f);
	alSourcef(HQMusicSource, AL_GAIN, MusicVolume / 127.0f);
	alSourcei(HQMusicSource, AL_LOOPING, loop);
	I_ErrorCheck("Creating HQ music source");

	alGenBuffers(1, &HQMusicBuffer);
	alBufferData(HQMusicBuffer, AL_FORMAT_STEREO_FLOAT32, (ALvoid*)song_data.data(), song_data.size() * sizeof(float), sample_rate);
	I_ErrorCheck("Creating HQ music buffer");
	alSourcei(HQMusicSource, AL_BUFFER, HQMusicBuffer);
	alSourcePlay(HQMusicSource);
	I_ErrorCheck("Playing HQ music");
	HQMusicPlaying = true;
}

void I_StopHQSong()
{
	if (HQMusicPlaying)
	{
		alSourceStop(HQMusicSource);
		alDeleteSources(1, &HQMusicSource);
		alDeleteBuffers(1, &HQMusicBuffer);
		I_ErrorCheck("Stopping HQ music");
		HQMusicPlaying = false;
	}
}

void I_CreateMusicSource()
{
	alGenSources(1, &MusicSource);
	alSourcef(MusicSource, AL_ROLLOFF_FACTOR, 0.0f);
	alSource3f(MusicSource, AL_POSITION, 1.0f, 0.0f, 0.0f);
	alSourcef(MusicSource, AL_GAIN, MusicVolume / 127.0f);
	memset(&BufferQueue[0], 0, sizeof(ALuint) * MAX_BUFFERS_QUEUED);
	I_ErrorCheck("Creating music source");
	if (!alIsSource(MusicSource))
	{
		fprintf(stderr, "what the fuck\n");
		Int3();
	}

/*	//Immediately kick off the first buffer if possible
	int finalTicks = S_SequencerRender(S_GetTicksPerSecond(), MusicBufferData);
	alGenBuffers(1, &BufferQueue[0]);
	alBufferData(BufferQueue[0], AL_FORMAT_STEREO16, MusicBufferData, finalTicks * S_GetSamplesPerTick() * sizeof(ALushort) * 2, 44100);
	alSourceQueueBuffers(MusicSource, 1, &BufferQueue[0]);
	alSourcePlay(MusicSource);
	I_ErrorCheck("Queueing music buffers");*/
}

void I_DestroyMusicSource()
{
	int BuffersProcessed;
	alGetSourcei(MusicSource, AL_BUFFERS_QUEUED, &BuffersProcessed);
	/*if (BuffersProcessed > 0) //Free any lingering buffers
	{
		alSourceUnqueueBuffers(MusicSource, BuffersProcessed, &BufferQueue[0]);
		alDeleteBuffers(BuffersProcessed, &BufferQueue[0]);
	}
	I_ErrorCheck("Destroying lingering music buffers");*/
	alDeleteSources(1, &MusicSource);
	I_ErrorCheck("Destroying music source");
	alDeleteBuffers(BuffersProcessed, &BufferQueue[0]);
	I_ErrorCheck("Destroying lingering buffers");
	free(MusicBufferData);
	MusicSource = 0;
}

bool I_CanQueueMusicBuffer()
{
	if (!alIsSource(MusicSource))
	{
		Int3();
	}
	alGetSourcei(MusicSource, AL_BUFFERS_QUEUED, &CurrentBuffers);
	I_ErrorCheck("Checking can queue buffers");
	return CurrentBuffers < MAX_BUFFERS_QUEUED;
}

void I_DequeueMusicBuffers()
{
	int BuffersProcessed;
	alGetSourcei(MusicSource, AL_BUFFERS_PROCESSED, &BuffersProcessed);
	if (BuffersProcessed > 0)
	{
		alSourceUnqueueBuffers(MusicSource, BuffersProcessed, &BufferQueue[0]);
		alDeleteBuffers(BuffersProcessed, &BufferQueue[0]);
		for (int i = 0; i < MAX_BUFFERS_QUEUED - BuffersProcessed; i++)
		{
			BufferQueue[i] = BufferQueue[i + BuffersProcessed];
		}
		//printf("Killing %d buffers\n", BuffersProcessed);
	}
	I_ErrorCheck("Unqueueing music buffers");
}

void I_QueueMusicBuffer(int numTicks, uint16_t *data)
{
	//printf("Queuing %d ticks\n", numTicks);
	alGetSourcei(MusicSource, AL_BUFFERS_QUEUED, &CurrentBuffers);
	if (CurrentBuffers < MAX_BUFFERS_QUEUED)
	{
		//int finalTicks = S_SequencerRender(S_GetTicksPerSecond(), MusicBufferData);
		alGenBuffers(1, &BufferQueue[CurrentBuffers]);
		alBufferData(BufferQueue[CurrentBuffers], AL_FORMAT_STEREO16, data, numTicks * MIDI_SAMPLESPERTICK * sizeof(ALushort) * 2, MIDI_SAMPLERATE);
		alSourceQueueBuffers(MusicSource, 1, &BufferQueue[CurrentBuffers]);
		I_ErrorCheck("Queueing music buffers");
	}
	if (!playing)
	{
		playing = true;
		alSourcePlay(MusicSource);
		I_ErrorCheck("Playing music source");
		//printf("Kicking this mess off\n");
	}
}

void I_MIDIThread()
{
/*	std::unique_lock<std::mutex> lock(MIDIMutex);
	S_StartMIDISong(CurrentSong, LoopMusic);
	I_CreateMusicSource();
	while (!StopMIDI)
	{
		lock.unlock();
		I_QueueMusicBuffer();
		lock.lock();
	}
	S_StopSequencer();
	I_DestroyMusicSource();*/
}

void I_StartMIDISong()
{
	StopMIDI = false;
	playing = false;
	I_CreateMusicSource();
	I_ErrorCheck("Creating source");
}

void I_StopMIDISong()
{
	StopMIDI = true;
	alSourceStop(MusicSource);
	I_DequeueMusicBuffers();
	I_DestroyMusicSource();
	I_ErrorCheck("Destroying source");
}

//-----------------------------------------------------------------------------
// Emitting buffered movie sound at player
//-----------------------------------------------------------------------------
#define NUMMVESNDBUFFERS 100

//Next buffer position, and earliest buffer position still currently queued
int mveSndBufferHead, mveSndBufferTail;
//Ring buffer of all the buffer names
ALuint mveSndRingBuffer[NUMMVESNDBUFFERS];

ALenum mveSndFormat;
ALint mveSndSampleRate;
ALuint mveSndSourceName;
ALboolean mveSndPlaying;

void I_CreateMovieSource()
{
	alGenSources(1, &mveSndSourceName);
	alSourcef(mveSndSourceName, AL_ROLLOFF_FACTOR, 0.0f);
	alSource3f(mveSndSourceName, AL_POSITION, 1.0f, 0.0f, 0.0f);
	I_ErrorCheck("Creating movie source");
}

void I_InitMovieAudio(int format, int samplerate, int stereo)
{
	//printf("format: %d, samplerate: %d, stereo: %d\n", format, samplerate, stereo);
	switch (format)
	{
	case MVESND_U8:
		if (stereo)
			mveSndFormat = AL_FORMAT_STEREO8;
		else
			mveSndFormat = AL_FORMAT_MONO8;
		break;
	case MVESND_S16LSB:
		if (stereo)
			mveSndFormat = AL_FORMAT_STEREO16;
		else
			mveSndFormat = AL_FORMAT_MONO16;
		break;
	}
	mveSndSampleRate = samplerate;

	mveSndBufferHead = 0; mveSndBufferTail = 0;
	mveSndPlaying = AL_FALSE;

	I_CreateMovieSource();
}

void I_DequeueMovieAudioBuffers(int all)
{
	int i, n;
	//Dequeue processed buffers in the ring buffer
	alGetSourcei(mveSndSourceName, AL_BUFFERS_PROCESSED, &n);
	for (i = 0; i < n; i++)
	{
		alSourceUnqueueBuffers(mveSndSourceName, 1, &mveSndRingBuffer[mveSndBufferTail]);
		alDeleteBuffers(1, &mveSndRingBuffer[mveSndBufferTail]);

		mveSndBufferTail++;
		if (mveSndBufferTail == NUMMVESNDBUFFERS)
			mveSndBufferTail = 0;
	}
	I_ErrorCheck("Dequeing movie buffers");
	//kill all remaining buffers if we're told to stop
	if (all)
	{
		alGetSourcei(mveSndSourceName, AL_BUFFERS_QUEUED, &n);
		for (i = 0; i < n; i++)
		{
			alSourceUnqueueBuffers(mveSndSourceName, 1, &mveSndRingBuffer[mveSndBufferTail]);
			alDeleteBuffers(1, &mveSndRingBuffer[mveSndBufferTail]);

			mveSndBufferTail++;
			if (mveSndBufferTail == NUMMVESNDBUFFERS)
				mveSndBufferTail = 0;
		}
		I_ErrorCheck("Dequeing excess movie buffers");
	}
}

void I_QueueMovieAudioBuffer(int len, short* data)
{
	//I don't currently have a tick function (should I fix this?), so do this now
	I_DequeueMovieAudioBuffers(0);

	//Generate and fill out a new buffer
	alGenBuffers(1, &mveSndRingBuffer[mveSndBufferHead]);
	alBufferData(mveSndRingBuffer[mveSndBufferHead], mveSndFormat, (ALvoid*)data, len, mveSndSampleRate);

	I_ErrorCheck("Creating movie buffers");
	//Queue the buffer, and if this source isn't playing, kick it off
	alSourceQueueBuffers(mveSndSourceName, 1, &mveSndRingBuffer[mveSndBufferHead]);
	if (mveSndPlaying == AL_FALSE)
	{
		alSourcePlay(mveSndSourceName);
		mveSndPlaying = AL_TRUE;
	}

	mveSndBufferHead++;
	if (mveSndBufferHead == NUMMVESNDBUFFERS)
		mveSndBufferHead = 0;

	I_ErrorCheck("Queuing movie buffers");
}

void I_DestroyMovieAudio()
{
	if (mveSndPlaying)
		alSourceStop(mveSndSourceName);

	I_DequeueMovieAudioBuffers(1);

	alDeleteSources(1, &mveSndSourceName);
}

void I_PauseMovieAudio()
{
	alSourcePause(mveSndSourceName);
}

void I_UnPauseMovieAudio()
{
	alSourcePlay(mveSndSourceName);
}

#endif
