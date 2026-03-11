#include "test_wav_loader.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct WavFmtChunk
{
  uint16_t audio_format;
  uint16_t channels;
  uint32_t sample_rate;
  uint16_t bits_per_sample;
} WavFmtChunk;

static uint16_t read_le_u16(const uint8_t* p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le_u32(const uint8_t* p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int read_exact(FILE* f, void* dst, size_t size)
{
  return fread(dst, 1u, size, f) == size ? 0 : -1;
}

static int skip_bytes(FILE* f, uint32_t size)
{
  if (size == 0u) {
    return 0;
  }
  return fseek(f, (long)size, SEEK_CUR) == 0 ? 0 : -1;
}

static void test_wav_reset(TestWavData* wav)
{
  if (!wav) {
    return;
  }
  wav->left        = NULL;
  wav->right       = NULL;
  wav->frames      = 0u;
  wav->sample_rate = 0u;
  wav->channels    = 0u;
}

void test_wav_free(TestWavData* wav)
{
  if (!wav) {
    return;
  }
  free(wav->left);
  free(wav->right);
  test_wav_reset(wav);
}

static int parse_fmt_chunk(const uint8_t* buf, uint32_t size, WavFmtChunk* fmt)
{
  if (!buf || !fmt || size < 16u) {
    return -1;
  }

  fmt->audio_format    = read_le_u16(buf + 0u);
  fmt->channels        = read_le_u16(buf + 2u);
  fmt->sample_rate     = read_le_u32(buf + 4u);
  fmt->bits_per_sample = read_le_u16(buf + 14u);

  if (fmt->audio_format != 1u) {
    return -1;
  }
  if (!(fmt->channels == 1u || fmt->channels == 2u)) {
    return -1;
  }
  if (fmt->sample_rate == 0u) {
    return -1;
  }
  if (fmt->bits_per_sample != 16u) {
    return -1;
  }

  return 0;
}

static int load_pcm16_data(FILE* f, uint32_t data_size, const WavFmtChunk* fmt, TestWavData* out)
{
  uint8_t* raw    = NULL;
  float*   left   = NULL;
  float*   right  = NULL;
  size_t   frames = 0u;
  size_t   i;

  if (!f || !fmt || !out) {
    return -1;
  }

  {
    const uint32_t bytes_per_sample = (uint32_t)(fmt->bits_per_sample / 8u);
    const uint32_t frame_bytes      = (uint32_t)fmt->channels * bytes_per_sample;
    if (frame_bytes == 0u || (data_size % frame_bytes) != 0u) {
      return -1;
    }
    frames = (size_t)(data_size / frame_bytes);
  }

  if (frames == 0u) {
    return -1;
  }

  raw   = (uint8_t*)malloc((size_t)data_size);
  left  = (float*)calloc(frames, sizeof(float));
  right = (float*)calloc(frames, sizeof(float));
  if (!raw || !left || !right) {
    free(raw);
    free(left);
    free(right);
    return -1;
  }

  if (read_exact(f, raw, (size_t)data_size) != 0) {
    free(raw);
    free(left);
    free(right);
    return -1;
  }

  if (fmt->channels == 1u) {
    for (i = 0u; i < frames; ++i) {
      const int16_t s = (int16_t)read_le_u16(raw + i * 2u);
      const float   v = (float)s / 32768.0f;
      left[i]         = v;
      right[i]        = v;
    }
  } else {
    for (i = 0u; i < frames; ++i) {
      const size_t  off = i * 4u;
      const int16_t sl  = (int16_t)read_le_u16(raw + off + 0u);
      const int16_t sr  = (int16_t)read_le_u16(raw + off + 2u);
      left[i]           = (float)sl / 32768.0f;
      right[i]          = (float)sr / 32768.0f;
    }
  }

  free(raw);

  out->left        = left;
  out->right       = right;
  out->frames      = frames;
  out->sample_rate = fmt->sample_rate;
  out->channels    = fmt->channels;
  return 0;
}

int test_wav_load(const char* path, TestWavData* out)
{
  FILE*       f = NULL;
  uint8_t     riff_header[12];
  int         have_fmt  = 0;
  int         have_data = 0;
  WavFmtChunk fmt;

  if (!path || !out) {
    return -1;
  }

  test_wav_reset(out);
  memset(&fmt, 0, sizeof(fmt));

  f = fopen(path, "rb");
  if (!f) {
    return -1;
  }

  if (read_exact(f, riff_header, sizeof(riff_header)) != 0) {
    fclose(f);
    return -1;
  }

  if (memcmp(riff_header + 0u, "RIFF", 4u) != 0 || memcmp(riff_header + 8u, "WAVE", 4u) != 0) {
    fclose(f);
    return -1;
  }

  while (!have_data) {
    uint8_t  chunk_header[8];
    uint32_t chunk_size;
    uint32_t padded_size;

    if (read_exact(f, chunk_header, sizeof(chunk_header)) != 0) {
      break;
    }

    chunk_size  = read_le_u32(chunk_header + 4u);
    padded_size = chunk_size + (chunk_size & 1u);

    if (memcmp(chunk_header + 0u, "fmt ", 4u) == 0) {
      uint8_t* fmt_buf = (uint8_t*)malloc((size_t)chunk_size);
      if (!fmt_buf) {
        fclose(f);
        return -1;
      }
      if (read_exact(f, fmt_buf, (size_t)chunk_size) != 0) {
        free(fmt_buf);
        fclose(f);
        return -1;
      }
      if (parse_fmt_chunk(fmt_buf, chunk_size, &fmt) != 0) {
        free(fmt_buf);
        fclose(f);
        return -1;
      }
      free(fmt_buf);
      have_fmt = 1;

      if ((padded_size - chunk_size) != 0u && skip_bytes(f, padded_size - chunk_size) != 0) {
        fclose(f);
        return -1;
      }
    } else if (memcmp(chunk_header + 0u, "data", 4u) == 0) {
      if (!have_fmt) {
        fclose(f);
        return -1;
      }
      if (load_pcm16_data(f, chunk_size, &fmt, out) != 0) {
        fclose(f);
        return -1;
      }
      have_data = 1;

      if ((padded_size - chunk_size) != 0u && skip_bytes(f, padded_size - chunk_size) != 0) {
        test_wav_free(out);
        fclose(f);
        return -1;
      }
    } else {
      if (skip_bytes(f, padded_size) != 0) {
        fclose(f);
        return -1;
      }
    }
  }

  fclose(f);

  if (!have_fmt || !have_data || !out->left || !out->right || out->frames == 0u) {
    test_wav_free(out);
    return -1;
  }

  return 0;
}