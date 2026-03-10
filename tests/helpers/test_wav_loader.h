#ifndef TEST_WAV_LOADER_H
#define TEST_WAV_LOADER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TestWavData {
    float* left;
    float* right;
    size_t frames;
    unsigned sample_rate;
    unsigned channels;
} TestWavData;

/* Load a little-endian RIFF/WAVE PCM file for unit tests.
 * Supported formats:
 *   - PCM integer, 16-bit
 *   - mono or stereo
 *
 * Mono files populate both left and right with the same samples so tests can
 * treat the result uniformly as stereo.
 *
 * Returns 0 on success, non-zero on failure.
 */
int test_wav_load(const char* path, TestWavData* out);

/* Release memory allocated by test_wav_load and reset the struct. */
void test_wav_free(TestWavData* wav);

#ifdef __cplusplus
}
#endif

#endif /* TEST_WAV_LOADER_H */