/*
 Copyright (c) 2023, Dominic Szablewski - https://phoboslab.org
 SPDX-License-Identifier: MIT
 QOA - The "Quite OK Audio" format for fast, lossy audio compression
 -- Data Format
 A QOA file has an 8 byte file header, followed by a number of frames. Each frame
 consists of an 8 byte frame header, the current 8 byte en-/decoder state per
 channel and 256 slices per channel. Each slice is 8 bytes wide and encodes 20
 samples of audio data.
 Note that the last frame of a file may contain less than 256 slices per channel.
 The last slice (per channel) in the last frame may contain less 20 samples, but
 the slice will still be 8 bytes wide, with the unused samples zeroed out.
 The samplerate and number of channels is only stated in the frame headers, but
 not in the file header. A decoder may peek into the first frame of the file to
 find these values.
 In a valid QOA file all frames have the same number of channels and the same
 samplerate. These restrictions may be relaxed for streaming. This remains to
 be decided.
 All values in a QOA file are BIG ENDIAN. Luckily, EVERYTHING in a QOA file,
 including the headers, is 64 bit aligned, so it's possible to read files with
 just a read_u64() that does the byte swapping if necessary.
 In pseudocode, the file layout is as follows:
 struct {
 struct {
 char     magic[4];         // magic bytes 'qoaf'
 uint32_t samples;          // number of samples per channel in this file
 } file_header;                 // = 64 bits
 struct {
 struct {
 uint8_t  num_channels; // number of channels
 uint24_t samplerate;   // samplerate in hz
 uint16_t fsamples;     // sample count per channel in this frame
 uint16_t fsize;        // frame size (including the frame header)
 } frame_header;            // = 64 bits
 struct {
 int16_t history[4];    // = 64 bits
 int16_t weights[4];    // = 64 bits
 } lms_state[num_channels];
 qoa_slice_t slices[256][num_channels]; // = 64 bits each
 } frames[samples * channels / qoa_max_framesize()];
 } qoa_file;
 Wheras the 64bit qoa_slice_t is defined as follows:
 .- QOA_SLICE -- 64 bits, 20 samples --------------------------/  /------------.
 |        Byte[0]         |        Byte[1]         |  Byte[2]  \  \  Byte[7]   |
 | 7  6  5  4  3  2  1  0 | 7  6  5  4  3  2  1  0 | 7  6  5   /  /    2  1  0 |
 |------------+--------+--------+--------+---------+---------+-\  \--+---------|
 |  sf_index  |  r00   |   r01  |   r02  |  r03    |   r04   | /  /  |   r19   |
 `-------------------------------------------------------------\  \------------`
 `sf_index` defines the scalefactor to use for this slice as an index into the
 qoa_scalefactor_tab[16]
 `r00`--`r19` are the residuals for the individual samples, divided by the
 scalefactor and quantized by the qoa_quant_tab[].
 In the decoder, a prediction of the next sample is computed by multiplying the
 state (the last four output samples) with the predictor. The residual from the
 slice is then dequantized using the qoa_dequant_tab[] and added to the
 prediction. The result is clamped to int16 to form the final output sample.
 */



/* -----------------------------------------------------------------------------
 Header - Public functions */

#ifndef QOA_H
#define QOA_H

#define QOA_MIN_FILESIZE 16
#define QOA_MAX_CHANNELS 8

#define QOA_SLICE_LEN 20
#define QOA_SLICES_PER_FRAME 256
#define QOA_FRAME_LEN (QOA_SLICES_PER_FRAME * QOA_SLICE_LEN)
#define QOA_LMS_LEN 4
#define QOA_MAGIC 0x716f6166 /* 'qoaf' */

#define QOA_FRAME_SIZE(channels, slices) \
  (8 + QOA_LMS_LEN * 4 * channels + 8 * slices * channels)

typedef struct {
  int history[QOA_LMS_LEN];
  int weights[QOA_LMS_LEN];
} qoa_lms_t;

  typedef struct {
    unsigned int channels;
    unsigned int samplerate;
    unsigned int samples;
    qoa_lms_t lms[QOA_MAX_CHANNELS];
#ifdef QOA_RECORD_TOTAL_ERROR
    double error;
#endif
  } qoa_desc;

  unsigned int qoa_encode_header(qoa_desc *qoa, unsigned char *bytes);
  unsigned int qoa_encode_frame(const short *sample_data, qoa_desc *qoa, unsigned int frame_len, unsigned char *bytes);
  void *qoa_encode(const short *sample_data, qoa_desc *qoa, unsigned int *out_len);

  unsigned int qoa_max_frame_size(qoa_desc *qoa);
  unsigned int qoa_decode_header(const unsigned char *bytes, int size, qoa_desc *qoa);
  unsigned int qoa_decode_frame(const unsigned char *bytes, unsigned int size, qoa_desc *qoa, short *sample_data, unsigned int *frame_len);
  short *qoa_decode(const unsigned char *bytes, int size, qoa_desc *file);

  int qoa_write(const char *filename, const short *sample_data, qoa_desc *qoa);
  void *qoa_read(const char *filename, qoa_desc *qoa);

#endif /* QOA_H */


/* -----------------------------------------------------------------------------
 Implementation */

#include <stdlib.h>

#ifndef QOA_MALLOC
#define QOA_MALLOC(sz) malloc(sz)
#define QOA_FREE(p) free(p)
#endif

typedef unsigned long long qoa_uint64_t;


/* The quant_tab provides an index into the dequant_tab for residuals in the
 range of -8 .. 8. It maps this range to just 3bits and becomes less accurate at
 the higher end. Note that the residual zero is identical to the lowest positive
 value. This is mostly fine, since the qoa_div() function always rounds away
 from zero. */

static const int qoa_quant_tab[17] = {
  7, 7, 7, 5, 5, 3, 3, 1, /* -8..-1 */
0,                      /*  0     */
0, 2, 2, 4, 4, 6, 6, 6  /*  1.. 8 */
};


/* We have 16 different scalefactors. Like the quantized residuals these become
 less accurate at the higher end. In theory, the highest scalefactor that we
 would need to encode the highest 16bit residual is (2**16)/8 = 8192. However we
 rely on the LMS filter to predict samples accurately enough that a maximum
 residual of one quarter of the 16 bit range is high sufficient. I.e. with the
 scalefactor 2048 times the quant range of 8 we can encode residuals up to 2**14.
 The scalefactor values are computed as:
 scalefactor_tab[s] <- round(pow(s + 1, 2.75)) */

static const int qoa_scalefactor_tab[16] = {
  1, 7, 21, 45, 84, 138, 211, 304, 421, 562, 731, 928, 1157, 1419, 1715, 2048
};


/* The reciprocal_tab maps each of the 16 scalefactors to their rounded
 reciprocals 1/scalefactor. This allows us to calculate the scaled residuals in
 the encoder with just one multiplication instead of an expensive division. We
 do this in .16 fixed point with integers, instead of floats.
 The reciprocal_tab is computed as:
 reciprocal_tab[s] <- ((1<<16) + scalefactor_tab[s] - 1) / scalefactor_tab[s] */

static const int qoa_reciprocal_tab[16] = {
  65536, 9363, 3121, 1457, 781, 475, 311, 216, 156, 117, 90, 71, 57, 47, 39, 32
};


/* The dequant_tab maps each of the scalefactors and quantized residuals to
 their unscaled & dequantized version.
 Since qoa_div rounds away from the zero, the smallest entries are mapped to 3/4
 instead of 1. The dequant_tab assumes the following dequantized values for each
 of the quant_tab indices and is computed as:
 float dqt[8] = {0.75, -0.75, 2.5, -2.5, 4.5, -4.5, 7, -7};
 dequant_tab[s][q] <- round(scalefactor_tab[s] * dqt[q]) */

static const int qoa_dequant_tab[16][8] = {
  {   1,    -1,    3,    -3,    5,    -5,     7,     -7},
  {   5,    -5,   18,   -18,   32,   -32,    49,    -49},
  {  16,   -16,   53,   -53,   95,   -95,   147,   -147},
  {  34,   -34,  113,  -113,  203,  -203,   315,   -315},
  {  63,   -63,  210,  -210,  378,  -378,   588,   -588},
  { 104,  -104,  345,  -345,  621,  -621,   966,   -966},
  { 158,  -158,  528,  -528,  950,  -950,  1477,  -1477},
  { 228,  -228,  760,  -760, 1368, -1368,  2128,  -2128},
  { 316,  -316, 1053, -1053, 1895, -1895,  2947,  -2947},
  { 422,  -422, 1405, -1405, 2529, -2529,  3934,  -3934},
  { 548,  -548, 1828, -1828, 3290, -3290,  5117,  -5117},
  { 696,  -696, 2320, -2320, 4176, -4176,  6496,  -6496},
  { 868,  -868, 2893, -2893, 5207, -5207,  8099,  -8099},
  {1064, -1064, 3548, -3548, 6386, -6386,  9933,  -9933},
  {1286, -1286, 4288, -4288, 7718, -7718, 12005, -12005},
  {1536, -1536, 5120, -5120, 9216, -9216, 14336, -14336},
};


/* The Least Mean Squares Filter is the heart of QOA. It predicts the next
 sample based on the previous 4 reconstructed samples. It does so by continuously
 adjusting 4 weights based on the residual of the previous prediction.
 The next sample is predicted as the sum of (weight[i] * history[i]).
 The adjustment of the weights is done with a "Sign-Sign-LMS" that adds or
 subtracts the residual to each weight, based on the corresponding sample from
 the history. This, surprisingly, is sufficient to get worthwhile predictions.
 This is all done with fixed point integers. Hence the right-shifts when updating
 the weights and calculating the prediction. */

static int qoa_lms_predict(qoa_lms_t *lms) {
  int prediction = 0;
  for (int i = 0; i < QOA_LMS_LEN; i++) {
    prediction += lms->weights[i] * lms->history[i];
  }
  return prediction >> 13;
}

static void qoa_lms_update(qoa_lms_t *lms, int sample, int residual) {
  int delta = residual >> 4;
  for (int i = 0; i < QOA_LMS_LEN; i++) {
    lms->weights[i] += lms->history[i] < 0 ? -delta : delta;
  }

  for (int i = 0; i < QOA_LMS_LEN-1; i++) {
    lms->history[i] = lms->history[i+1];
  }
  lms->history[QOA_LMS_LEN-1] = sample;
}


/* qoa_div() implements a rounding division, but avoids rounding to zero for
 small numbers. E.g. 0.1 will be rounded to 1. Note that 0 itself still
 returns as 0, which is handled in the qoa_quant_tab[].
 qoa_div() takes an index into the .16 fixed point qoa_reciprocal_tab as an
 argument, so it can do the division with a cheaper integer multiplication. */

static inline int qoa_div(int v, int scalefactor) {
  int reciprocal = qoa_reciprocal_tab[scalefactor];
  int n = (v * reciprocal + (1 << 15)) >> 16;
  n = n + ((v > 0) - (v < 0)) - ((n > 0) - (n < 0)); /* round away from 0 */
return n;
}

static inline int qoa_clamp(int v, int min, int max) {
  return (v < min) ? min : (v > max) ? max : v;
}

static inline qoa_uint64_t qoa_read_u64(const unsigned char *bytes, unsigned int *p) {
  bytes += *p;
  *p += 8;
  return
  ((qoa_uint64_t)(bytes[0]) << 56) | ((qoa_uint64_t)(bytes[1]) << 48) |
    ((qoa_uint64_t)(bytes[2]) << 40) | ((qoa_uint64_t)(bytes[3]) << 32) |
    ((qoa_uint64_t)(bytes[4]) << 24) | ((qoa_uint64_t)(bytes[5]) << 16) |
    ((qoa_uint64_t)(bytes[6]) <<  8) | ((qoa_uint64_t)(bytes[7]) <<  0);
}

static inline void qoa_write_u64(qoa_uint64_t v, unsigned char *bytes, unsigned int *p) {
  bytes += *p;
  *p += 8;
  bytes[0] = (v >> 56) & 0xff;
  bytes[1] = (v >> 48) & 0xff;
  bytes[2] = (v >> 40) & 0xff;
  bytes[3] = (v >> 32) & 0xff;
  bytes[4] = (v >> 24) & 0xff;
  bytes[5] = (v >> 16) & 0xff;
  bytes[6] = (v >>  8) & 0xff;
  bytes[7] = (v >>  0) & 0xff;
}
