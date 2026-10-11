/* coreaudiofilltest.c: exercise the actual Core Audio backend with fake units.
   GPL version 2 or later. No audio device is opened by these tests. */
#include "config.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <AudioToolbox/AudioToolbox.h>
#ifdef COREAUDIO_TEST_THREADS
#include <pthread.h>
#endif

#include "machine.h"
#include "ui/ui.h"

#define CHECK(x) do { if( !(x) ) { \
  fprintf( stderr, "coreaudiofilltest:%d: %s\n", __LINE__, #x ); exit( 1 ); \
} } while( 0 )

static int failure_stage, call_stage, live_units;
static int device_missing;
static double sample_rate = 48000;
static libspectrum_dword processor_speed = 3500000;
static int stop_fails, dispose_fails;

static OSStatus
mock_property( AudioObjectID object, const AudioObjectPropertyAddress *address,
               UInt32 qualifier_size, const void *qualifier,
               UInt32 *size, void *data )
{
  (void)object; (void)qualifier_size; (void)qualifier; (void)size;
  if( ++call_stage == failure_stage ) return -1;
  if( address->mSelector == kAudioHardwarePropertyDefaultOutputDevice )
    *(AudioDeviceID *)data = device_missing ? kAudioObjectUnknown : 1;
  else
    *(Float64 *)data = sample_rate;
  return noErr;
}

static AudioComponent
mock_find( AudioComponent previous, const AudioComponentDescription *desc )
{
  (void)previous; (void)desc;
  return ++call_stage == failure_stage ? NULL : (AudioComponent)(uintptr_t)1;
}

static OSStatus
mock_new( AudioComponent component, AudioComponentInstance *unit )
{
  (void)component;
  if( ++call_stage == failure_stage ) return -1;
  *unit = (AudioUnit)(uintptr_t)1;
  live_units++;
  return noErr;
}

static OSStatus
mock_set( AudioUnit unit, AudioUnitPropertyID property, AudioUnitScope scope,
          AudioUnitElement element, const void *data, UInt32 size )
{
  (void)unit; (void)property; (void)scope; (void)element; (void)data; (void)size;
  return ++call_stage == failure_stage ? -1 : noErr;
}

static OSStatus
mock_initialize( AudioUnit unit )
{
  (void)unit;
  return ++call_stage == failure_stage ? -1 : noErr;
}

static OSStatus
mock_uninitialize( AudioUnit unit )
{
  (void)unit;
  return noErr;
}

static OSStatus
mock_dispose( AudioComponentInstance unit )
{
  (void)unit;
  if( dispose_fails ) return -1;
  live_units--;
  return noErr;
}

static OSStatus
mock_start( AudioUnit unit )
{
  (void)unit;
  return noErr;
}

static OSStatus
mock_stop( AudioUnit unit )
{
  (void)unit;
  return stop_fails ? -1 : noErr;
}

#define AudioObjectGetPropertyData mock_property
#define AudioComponentFindNext mock_find
#define AudioComponentInstanceNew mock_new
#define AudioUnitSetProperty mock_set
#define AudioUnitInitialize mock_initialize
#define AudioUnitUninitialize mock_uninitialize
#define AudioComponentInstanceDispose mock_dispose
#define AudioOutputUnitStart mock_start
#define AudioOutputUnitStop mock_stop
static int release_frames, admission_waits;
static int mock_sleep( useconds_t microseconds );
#define usleep mock_sleep
/* Include rather than duplicate the production callback and fill algorithm. */
#include "sound/coreaudiosound.c"
#undef usleep

static int
mock_sleep( useconds_t microseconds )
{
  unsigned char output[128];
  if( !release_frames ) return usleep( microseconds );
  CHECK( microseconds == 10000 );
  admission_waits++;
  CHECK( fill_audio_frames( output, release_frames ) == release_frames );
  release_frames = 0;
  return 0;
}

static fuse_machine_info test_machine;
fuse_machine_info *machine_current = &test_machine;

libspectrum_dword
sound_get_effective_processor_speed( void )
{
  return processor_speed;
}

int
ui_error( ui_error_level severity, const char *format, ... )
{
  (void)severity; (void)format;
  return 0;
}

#define COUNTER(name) atomic_load_explicit( &audio_stats.name, memory_order_relaxed )

static void
setup_fifo( unsigned int channels, int capacity )
{
  device_format.mChannelsPerFrame = channels;
  device_format.mBytesPerFrame = channels * 2;
  CHECK( init_audio_stats() == 0 );
  CHECK( sfifo_init( &sound_fifo, capacity ) == 0 );
}

static void
admission_tests( void )
{
  unsigned char data[128] = { 0 };
  int channels, width, capacity;
  for( channels = 1; channels <= 2; channels++ ) {
    setup_fifo( channels, 31 );
    width = channels * 2;
    capacity = pcm_fifo_capacity( &sound_fifo, width );
    CHECK( !sound_lowlevel_reserve( capacity ) );
    CHECK( pcm_fifo_write( &sound_fifo, width, data, capacity ) == capacity );
    CHECK( !sound_lowlevel_reserve( 0 ) );
    CHECK( sound_lowlevel_reserve( capacity + 1 ) == -EINVAL );
    release_frames = 2;
    admission_waits = 0;
    CHECK( !sound_lowlevel_reserve( 2 ) && admission_waits == 1 );
    CHECK( pcm_fifo_write( &sound_fifo, width, data, 1 ) == 1 );
    CHECK( pcm_fifo_write( &sound_fifo, width, data, 1 ) == 1 );
    sfifo_close( &sound_fifo );
    CHECK( sound_lowlevel_reserve( 1 ) == -ENODEV );
  }
}

/* Timer reserves a maximum batch, not a channel-dependent byte count. */
static void
timer_capacity_tests( void )
{
  const int framesiz = 960;
  unsigned char data[8192] = { 0 };
  int channels, available_frames, available_bytes, reserve_bytes;

  for( channels = 1; channels <= 2; channels++ ) {
    setup_fifo( channels, 2048 * channels * 2 - 1 );
    reserve_bytes = framesiz * channels *
                    (int)sizeof( libspectrum_signed_word );
    for( available_frames = 0; available_frames <= 2047; available_frames++ ) {
      sfifo_flush( &sound_fifo );
      CHECK( sfifo_write( &sound_fifo, data,
                         ( 2047 - available_frames ) * channels * 2 ) ==
             ( 2047 - available_frames ) * channels * 2 );
      available_bytes = sfifo_producer_space( &sound_fifo );
      CHECK( available_bytes / ( channels * 2 ) == available_frames );
      CHECK( ( available_bytes < reserve_bytes ) ==
             ( available_frames < framesiz ) );
    }
    sfifo_close( &sound_fifo );
  }
}

static void
fill_tests( unsigned int channels )
{
  unsigned char input[80000], output[80004];
  AudioBufferList buffers;
  unsigned int i, bytes_per_frame = channels * 2;
  unsigned int requested = 80000 / bytes_per_frame;
  for( i = 0; i < sizeof( input ); i++ ) input[i] = ( i % 251 ) + 1;
  setup_fifo( channels, 100000 );
  buffers.mNumberBuffers = 1;
  buffers.mBuffers[0].mNumberChannels = channels;
  buffers.mBuffers[0].mDataByteSize = 80000;
  buffers.mBuffers[0].mData = output;

  /* Empty FIFO, large demand (>65535 bytes), and output guard bytes. */
  memset( output, 0xa5, sizeof( output ) );
  CHECK( coreaudiowrite( NULL, NULL, NULL, 0, requested, &buffers ) == noErr );
  for( i = 0; i < 80000; i++ ) CHECK( output[i] == 0 );
  CHECK( output[80000] == 0xa5 );
  CHECK( COUNTER( missing_frames ) == requested );
  CHECK( COUNTER( underrun_callbacks ) == 1 );

  /* Partial delivery must overwrite the entire shortage with zeros. */
  CHECK( write_audio_frames( input, 7 ) == 7 * bytes_per_frame );
  memset( output, 0xa5, sizeof( output ) );
  CHECK( coreaudiowrite( NULL, NULL, NULL, 0, 10, &buffers ) == noErr );
  CHECK( memcmp( input, output, 7 * bytes_per_frame ) == 0 );
  for( i = 7 * bytes_per_frame; i < 10 * bytes_per_frame; i++ )
    CHECK( output[i] == 0 );
  CHECK( output[10 * bytes_per_frame] == 0xa5 );
  CHECK( COUNTER( delivered_frames ) == 7 );
  CHECK( COUNTER( occupancy_frames ) == 7 );

  /* Exact large delivery, then excess queued audio. */
  CHECK( write_audio_frames( input, requested ) == 80000 );
  CHECK( coreaudiowrite( NULL, NULL, NULL, 0, requested, &buffers ) == noErr );
  CHECK( memcmp( input, output, 80000 ) == 0 );
  CHECK( COUNTER( maximum_occupancy_frames ) == requested );
  CHECK( write_audio_frames( input, 10 ) == 10 * bytes_per_frame );
  CHECK( coreaudiowrite( NULL, NULL, NULL, 0, 3, &buffers ) == noErr );
  CHECK( sfifo_consumer_used( &sound_fifo ) == 7 * bytes_per_frame );
  sfifo_flush( &sound_fifo );

  /* Modular unsigned totals; zero demand accepts a NULL data pointer. */
  atomic_store_explicit( &audio_stats.demand_frames, UINT_MAX, memory_order_relaxed );
  CHECK( coreaudiowrite( NULL, NULL, NULL, 0, 2, &buffers ) == noErr );
  CHECK( COUNTER( demand_frames ) == 1 );
  atomic_store_explicit( &audio_stats.delivered_frames, UINT_MAX, memory_order_relaxed );
  CHECK( write_audio_frames( input, 2 ) == 2 * bytes_per_frame );
  CHECK( coreaudiowrite( NULL, NULL, NULL, 0, 2, &buffers ) == noErr );
  CHECK( COUNTER( delivered_frames ) == 1 );
  atomic_store_explicit( &audio_stats.missing_frames, UINT_MAX, memory_order_relaxed );
  atomic_store_explicit( &audio_stats.underrun_callbacks, UINT_MAX, memory_order_relaxed );
  CHECK( coreaudiowrite( NULL, NULL, NULL, 0, 2, &buffers ) == noErr );
  CHECK( COUNTER( missing_frames ) == 1 );
  CHECK( COUNTER( underrun_callbacks ) == 0 );
  buffers.mBuffers[0].mData = NULL;
  CHECK( coreaudiowrite( NULL, NULL, NULL, 0, 0, &buffers ) == noErr );
  sfifo_close( &sound_fifo );

  /* Wrap both payload copies and keep partial producer transfers aligned. */
  setup_fifo( channels, 31 );
  for( i = 0; i < 1000; i++ ) {
    unsigned int frames = ( sound_fifo.size - 1 ) / bytes_per_frame;
    CHECK( write_audio_frames( input, 100 ) == frames * bytes_per_frame );
    CHECK( write_audio_frames( input, 1 ) == 0 );
    buffers.mBuffers[0].mData = output;
    CHECK( coreaudiowrite( NULL, NULL, NULL, 0, frames, &buffers ) == noErr );
    CHECK( memcmp( input, output, frames * bytes_per_frame ) == 0 );
    CHECK( sfifo_consumer_used( &sound_fifo ) == 0 );
  }
  sfifo_close( &sound_fifo );

  /* FIFO error is silence too. */
  memset( output, 0xa5, sizeof( output ) );
  CHECK( coreaudiowrite( NULL, NULL, NULL, 0, 10, &buffers ) == noErr );
  for( i = 0; i < 10 * bytes_per_frame; i++ ) CHECK( output[i] == 0 );
}

static void
invalid_tests( void )
{
  struct { UInt32 count; AudioBuffer buffer[2]; } storage;
  AudioBufferList *buffers = (AudioBufferList *)&storage;
  unsigned char input[16], output[16];
  unsigned int i;
  memset( input, 0x71, sizeof( input ) );
  setup_fifo( 2, 31 );
  CHECK( write_audio_frames( input, 4 ) == 16 );
  storage.count = 1;
  storage.buffer[0].mNumberChannels = 2;
  storage.buffer[0].mDataByteSize = sizeof( output );
  storage.buffer[0].mData = output;
  CHECK( coreaudiowrite( NULL, NULL, NULL, 0, 1, NULL ) == kAudio_ParamError );
  storage.count = 0;
  CHECK( coreaudiowrite( NULL, NULL, NULL, 0, 1, buffers ) == kAudio_ParamError );
  storage.count = 2;
  storage.buffer[1] = storage.buffer[0];
  CHECK( coreaudiowrite( NULL, NULL, NULL, 0, 1, buffers ) == kAudio_ParamError );
  storage.count = 1;
  storage.buffer[0].mNumberChannels = 1;
  CHECK( coreaudiowrite( NULL, NULL, NULL, 0, 1, buffers ) == kAudio_ParamError );
  storage.buffer[0].mNumberChannels = 2;
  storage.buffer[0].mData = NULL;
  CHECK( coreaudiowrite( NULL, NULL, NULL, 0, 1, buffers ) == kAudio_ParamError );
  storage.buffer[0].mData = output;
  storage.buffer[0].mDataByteSize = 3;
  memset( output, 0xa5, sizeof( output ) );
  CHECK( coreaudiowrite( NULL, NULL, NULL, 0, 1, buffers ) == kAudio_ParamError );
  for( i = 0; i < 3; i++ ) CHECK( output[i] == 0 );
  CHECK( output[3] == 0xa5 );
  CHECK( coreaudiowrite( NULL, NULL, NULL, 0, UINT_MAX, buffers ) == kAudio_ParamError );
  CHECK( sfifo_consumer_used( &sound_fifo ) == 16 );
  CHECK( COUNTER( invalid_callbacks ) == 7 );
  CHECK( COUNTER( delivered_frames ) == 0 );
  sfifo_close( &sound_fifo );
}

static void
initialization_tests( void )
{
  int stage, freq = 48000, stereo = 1;
  test_machine.timings.tstates_per_frame = 70000;
  for( stage = 1; stage <= 7; stage++ ) {
    failure_stage = stage;
    call_stage = 0;
    CHECK( sound_lowlevel_init( NULL, &freq, &stereo ) == 1 );
    CHECK( live_units == 0 && !output_unit && !sound_fifo.buffer );
  }
  failure_stage = 0;
  device_missing = 1;
  CHECK( sound_lowlevel_init( NULL, &freq, &stereo ) == 1 );
  device_missing = 0;
  sample_rate = 0;
  CHECK( sound_lowlevel_init( NULL, &freq, &stereo ) == 1 );
  sample_rate = NAN;
  CHECK( sound_lowlevel_init( NULL, &freq, &stereo ) == 1 );
  sample_rate = INFINITY;
  CHECK( sound_lowlevel_init( NULL, &freq, &stereo ) == 1 );
  sample_rate = (double)INT_MAX + 1;
  CHECK( sound_lowlevel_init( NULL, &freq, &stereo ) == 1 );
  sample_rate = 48000;
  processor_speed = 0;
  CHECK( sound_lowlevel_init( NULL, &freq, &stereo ) == 1 );
  processor_speed = 1;
  CHECK( sound_lowlevel_init( NULL, &freq, &stereo ) == 1 );
  processor_speed = 3500000;
  for( stage = 0; stage < 10; stage++ ) {
    libspectrum_signed_word samples[20] = { 0 };
    call_stage = 0;
    CHECK( sound_lowlevel_init( NULL, &freq, &stereo ) == 0 );
    /* Stereo capacity no longer has the duplicate channel multiplier. */
    CHECK( sound_fifo.size == 8192 );
    CHECK( !sound_lowlevel_reserve( 20 / device_format.mChannelsPerFrame ) );
    CHECK( !audio_output_started );
    sound_lowlevel_frame( samples, 20 );
    CHECK( audio_output_started );
    sound_lowlevel_end();
    CHECK( live_units == 0 && !output_unit && !sound_fifo.buffer );
  }
  CHECK( sound_lowlevel_init( NULL, &freq, &stereo ) == 0 );
  audio_output_started = 1;
  stop_fails = 1;
  sound_lowlevel_end();
  CHECK( sound_fifo.buffer && live_units == 1 );
  CHECK( sound_lowlevel_init( NULL, &freq, &stereo ) == 1 );
  stop_fails = 0;
  sound_lowlevel_end();
  audio_teardown_failed = 0; /* Test recovery, not a production retry policy. */
  CHECK( sound_lowlevel_init( NULL, &freq, &stereo ) == 0 );
  dispose_fails = 1;
  sound_lowlevel_end();
  CHECK( sound_fifo.buffer && live_units == 1 );
  CHECK( sound_lowlevel_init( NULL, &freq, &stereo ) == 1 );
  dispose_fails = 0;
  sound_lowlevel_end();
  audio_teardown_failed = 0;
}

#ifdef COREAUDIO_TEST_THREADS
static _Atomic unsigned int reader_done;
static void *
snapshot_reader( void *unused )
{
  (void)unused;
  while( !atomic_load_explicit( &reader_done, memory_order_relaxed ) ) {
    (void)COUNTER( demand_frames );
    (void)COUNTER( delivered_frames );
    (void)COUNTER( missing_frames );
    (void)COUNTER( underrun_callbacks );
    (void)COUNTER( occupancy_frames );
    (void)COUNTER( maximum_occupancy_frames );
    (void)COUNTER( invalid_callbacks );
  }
  return NULL;
}

#define CONCURRENT_FRAMES 200000u
static void *
frame_producer( void *unused )
{
  unsigned int pos = 0;
  unsigned char frame[4];
  int written;
  (void)unused;
  while( pos < CONCURRENT_FRAMES ) {
    frame[0] = pos % 251 + 1;
    frame[1] = ( pos / 251 ) % 251 + 1;
    frame[2] = 0x31;
    frame[3] = 0x73;
    written = write_audio_frames( frame, 1 );
    CHECK( written == 0 || written == 4 );
    if( written ) pos++;
  }
  return NULL;
}

static void
frame_concurrency_test( void )
{
  pthread_t producer;
  unsigned char output[12];
  AudioBufferList buffers = { 1, { { 2, sizeof( output ), output } } };
  unsigned int pos = 0, before, delivered, i;
  setup_fifo( 2, 31 );
  CHECK( pthread_create( &producer, NULL, frame_producer, NULL ) == 0 );
  while( pos < CONCURRENT_FRAMES ) {
    before = COUNTER( delivered_frames );
    memset( output, 0xa5, sizeof( output ) );
    CHECK( coreaudiowrite( NULL, NULL, NULL, 0, 3, &buffers ) == noErr );
    delivered = COUNTER( delivered_frames ) - before;
    CHECK( delivered <= 3 );
    for( i = 0; i < delivered; i++, pos++ ) {
      CHECK( output[i * 4] == pos % 251 + 1 );
      CHECK( output[i * 4 + 1] == ( pos / 251 ) % 251 + 1 );
      CHECK( output[i * 4 + 2] == 0x31 );
      CHECK( output[i * 4 + 3] == 0x73 );
    }
    for( i = delivered * 4; i < sizeof( output ); i++ ) CHECK( output[i] == 0 );
  }
  CHECK( pthread_join( producer, NULL ) == 0 );
  CHECK( sfifo_consumer_used( &sound_fifo ) == 0 );
  sfifo_close( &sound_fifo );
}

static void
snapshot_test( void )
{
  pthread_t reader;
  unsigned char output[16];
  AudioBufferList buffers = { 1, { { 2, sizeof( output ), output } } };
  int i;
  setup_fifo( 2, 31 );
  atomic_init( &reader_done, 0 );
  CHECK( pthread_create( &reader, NULL, snapshot_reader, NULL ) == 0 );
  for( i = 0; i < 100000; i++ )
    CHECK( coreaudiowrite( NULL, NULL, NULL, 0, 4, &buffers ) == noErr );
  atomic_store_explicit( &reader_done, 1, memory_order_relaxed );
  CHECK( pthread_join( reader, NULL ) == 0 );
  CHECK( COUNTER( demand_frames ) == 400000 );
  CHECK( COUNTER( missing_frames ) == 400000 );
  sfifo_close( &sound_fifo );
}
#endif

int
main( void )
{
  admission_tests();
  timer_capacity_tests();
  fill_tests( 1 );
  fill_tests( 2 );
  invalid_tests();
  initialization_tests();
#ifdef COREAUDIO_TEST_THREADS
  frame_concurrency_test();
  snapshot_test();
#else
  puts( "SKIP: pthread counter snapshot test" );
#endif
  puts( "coreaudiofilltest: passed" );
  return 0;
}
