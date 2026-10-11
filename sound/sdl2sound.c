/* sdl2sound.c: SDL 2 sound I/O
   Copyright (c) 2026 Fredrick Meunier

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
 */

#include "config.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#include <SDL.h>

#include "settings.h"
#include "pcm_fifo.h"
#include "sound.h"
#include "ui/ui.h"

static void sdl2write( void *userdata, Uint8 *stream, int len );

sfifo_t sound_fifo;

/* Number of Spectrum frames audio latency to use */
#define NUM_FRAMES 2

static SDL_AudioDeviceID audio_device;
static int audio_output_started;
/* Immutable while the device is open; obtained callback PCM geometry. */
static unsigned int audio_channels, bytes_per_frame;

int
sound_lowlevel_init( const char *device, int *freqptr, int *stereoptr )
{
  SDL_AudioSpec requested, received;
  int error;
  float hz;
  int sound_framesiz;

  if( audio_device || sound_fifo.buffer ) {
    ui_error( UI_ERROR_ERROR, "Previous SDL audio output has not been closed" );
    return 1;
  }

  if( device ) {
    error = SDL_setenv( "SDL_AUDIODRIVER", device, 1 );
    if( error ) {
      settings_current.sound = 0;
      ui_error( UI_ERROR_ERROR, "Couldn't set SDL_AUDIODRIVER: %s",
                SDL_GetError() );
      return 1;
    }
  }

  if( !SDL_WasInit( SDL_INIT_AUDIO ) && SDL_InitSubSystem( SDL_INIT_AUDIO ) ) {
    settings_current.sound = 0;
    ui_error( UI_ERROR_ERROR, "Couldn't initialize SDL audio: %s",
              SDL_GetError() );
    return 1;
  }

  memset( &requested, 0, sizeof( requested ) );

  requested.freq = *freqptr;
  requested.channels = *stereoptr ? 2 : 1;
  requested.format = AUDIO_S16SYS;
  requested.callback = sdl2write;

  hz = (float)sound_get_effective_processor_speed() /
       machine_current->timings.tstates_per_frame;
  if( hz > 100.0 ) hz = 100.0;
  sound_framesiz = *freqptr / hz;
#ifdef __FreeBSD__
  requested.samples = pow( 2.0, floor( log2( sound_framesiz ) ) );
#else
  requested.samples = sound_framesiz;
#endif

  audio_device = SDL_OpenAudioDevice( NULL, 0, &requested, &received,
                                      SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
                                      SDL_AUDIO_ALLOW_CHANNELS_CHANGE );
  if( !audio_device ) {
    settings_current.sound = 0;
    ui_error( UI_ERROR_ERROR, "Couldn't open sound device: %s",
              SDL_GetError() );
    return 1;
  }

  /* received describes callback PCM, not necessarily the hardware format.
     SDL may convert internally, but Fuse supplies only native signed 16-bit
     interleaved mono or stereo. Do not infer stereo from arbitrary channels. */
  if( received.format != AUDIO_S16SYS ||
      ( received.channels != 1 && received.channels != 2 ) ||
      received.freq <= 0 ) {
    SDL_CloseAudioDevice( audio_device );
    audio_device = 0;
    settings_current.sound = 0;
    ui_error( UI_ERROR_ERROR, "Unsupported SDL audio PCM specification" );
    return 1;
  }
  audio_channels = received.channels;
  bytes_per_frame = audio_channels * sizeof( libspectrum_signed_word );
  *freqptr = received.freq;
  *stereoptr = audio_channels == 2;

  sound_framesiz = *freqptr / hz;

  /* Preserve the historical byte allocation (including its extra byte) and
     sfifo rounding: physical capacity remains the buffering limit. */
  if( ( error = sfifo_init( &sound_fifo, NUM_FRAMES
                            * bytes_per_frame * sound_framesiz + 1 ) ) ) {
    SDL_CloseAudioDevice( audio_device );
    audio_device = 0;
    ui_error( UI_ERROR_ERROR, "Problem initialising sound fifo: %s",
              strerror( -error ) );
    return 1;
  }

  audio_output_started = 0;

  return 0;
}

void
sound_lowlevel_end( void )
{
  if( audio_device ) {
    SDL_PauseAudioDevice( audio_device, 1 );
    SDL_LockAudioDevice( audio_device );
    SDL_UnlockAudioDevice( audio_device );
    SDL_CloseAudioDevice( audio_device );
    audio_device = 0;
  }

  if( SDL_WasInit( SDL_INIT_AUDIO ) ) SDL_QuitSubSystem( SDL_INIT_AUDIO );

  sfifo_close( &sound_fifo );
  audio_output_started = 0;
  audio_channels = bytes_per_frame = 0;
}

int
sound_lowlevel_reserve( unsigned int frames )
{
  int admitted;
  while( !( admitted = pcm_fifo_producer_can_write(
              &sound_fifo, bytes_per_frame, frames ) ) )
    SDL_Delay( 10 );
  return admitted < 0 ? admitted : 0;
}

void
sound_lowlevel_frame( libspectrum_signed_word *data, int len )
{
  int i = 0;
  const char *bytes = (const char *)data;

  if( len < 0 || !audio_channels || len % audio_channels ) {
    ui_error( UI_ERROR_ERROR, "Invalid SDL audio sample count" );
    return;
  }
  len /= audio_channels;

  while( len ) {
    if( ( i = pcm_fifo_write( &sound_fifo, bytes_per_frame, bytes, len ) ) < 0 ) {
      break;
    } else if( !i ) {
      SDL_Delay( 10 );
    }

    bytes += i * bytes_per_frame;
    len -= i;
  }

  if( i < 0 ) {
    ui_error( UI_ERROR_ERROR, "Couldn't write sound fifo: %s",
              strerror( -i ) );
  }

  if( !audio_output_started && audio_device ) {
    SDL_PauseAudioDevice( audio_device, 0 );
    audio_output_started = 1;
  }
}

static void
sdl2write( void *userdata GCC_UNUSED, Uint8 *stream, int len )
{
  int delivered;

  if( len <= 0 || !stream ) return;
  /* SDL promises writable len-byte storage. A malformed frame request must
     neither consume queued PCM nor leave stale bytes in that storage. */
  if( !bytes_per_frame || len % bytes_per_frame ) {
    memset( stream, 0, len );
    return;
  }

  delivered = pcm_fifo_read( &sound_fifo, bytes_per_frame, stream,
                             len / bytes_per_frame );
  if( delivered < 0 ) delivered = 0;
  delivered *= bytes_per_frame;
  memset( stream + delivered, 0, len - delivered );
}
