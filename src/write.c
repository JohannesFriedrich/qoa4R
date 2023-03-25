#include <R.h>
#include <Rinternals.h>

#include <stdio.h>
#include "qoa.h"

unsigned int qoa_encode_header(qoa_desc *qoa, unsigned char *bytes) {
  unsigned int p = 0;
  qoa_write_u64(((qoa_uint64_t)QOA_MAGIC << 32) | qoa->samples, bytes, &p);
  return p;
}

unsigned int qoa_encode_frame(const short *sample_data, qoa_desc *qoa, unsigned int frame_len, unsigned char *bytes) {
  unsigned int channels = qoa->channels;

  unsigned int p = 0;
  unsigned int slices = (frame_len + QOA_SLICE_LEN - 1) / QOA_SLICE_LEN;
  unsigned int frame_size = QOA_FRAME_SIZE(channels, slices);

  /* Write the frame header */
  qoa_write_u64((
      (qoa_uint64_t)qoa->channels   << 56 |
        (qoa_uint64_t)qoa->samplerate << 32 |
        (qoa_uint64_t)frame_len       << 16 |
        (qoa_uint64_t)frame_size
  ), bytes, &p);

  /* Write the current LMS state */
  for (int c = 0; c < channels; c++) {
    qoa_uint64_t weights = 0;
    qoa_uint64_t history = 0;
    for (int i = 0; i < QOA_LMS_LEN; i++) {
      history = (history << 16) | (qoa->lms[c].history[i] & 0xffff);
      weights = (weights << 16) | (qoa->lms[c].weights[i] & 0xffff);
    }
    qoa_write_u64(history, bytes, &p);
    qoa_write_u64(weights, bytes, &p);
  }

  /* We encode all samples with the channels interleaved on a slice level.
   E.g. for stereo: (ch-0, slice 0), (ch 1, slice 0), (ch 0, slice 1), ...*/
  for (int sample_index = 0; sample_index < frame_len; sample_index += QOA_SLICE_LEN) {

    for (int c = 0; c < channels; c++) {
      int slice_len = qoa_clamp(QOA_SLICE_LEN, 0, frame_len - sample_index);
      int slice_start = sample_index * channels + c;
      int slice_end = (sample_index + slice_len) * channels + c;

      /* Brute for search for the best scalefactor. Just go through all
       16 scalefactors, encode all samples for the current slice and
       meassure the total squared error. */
      qoa_uint64_t best_error = -1;
      qoa_uint64_t best_slice;
      qoa_lms_t best_lms;

      for (int scalefactor = 0; scalefactor < 16; scalefactor++) {

        /* We have to reset the LMS state to the last known good one
         before trying each scalefactor, as each pass updates the LMS
         state when encoding. */
        qoa_lms_t lms = qoa->lms[c];
        qoa_uint64_t slice = scalefactor;
        qoa_uint64_t current_error = 0;

        for (int si = slice_start; si < slice_end; si += channels) {
          int sample = sample_data[si];
          int predicted = qoa_lms_predict(&lms);

          int residual = sample - predicted;
          int scaled = qoa_div(residual, scalefactor);
          int clamped = qoa_clamp(scaled, -8, 8);
          int quantized = qoa_quant_tab[clamped + 8];
          int dequantized = qoa_dequant_tab[scalefactor][quantized];
          int reconstructed = qoa_clamp(predicted + dequantized, -32768, 32767);

          long long error = (sample - reconstructed);
          current_error += error * error;
          if (current_error > best_error) {
            break;
          }

          qoa_lms_update(&lms, reconstructed, dequantized);
          slice = (slice << 3) | quantized;
        }

        if (current_error < best_error) {
          best_error = current_error;
          best_slice = slice;
          best_lms = lms;
        }
      }

      qoa->lms[c] = best_lms;
#ifdef QOA_RECORD_TOTAL_ERROR
      qoa->error += best_error;
#endif

      /* If this slice was shorter than QOA_SLICE_LEN, we have to left-
       shift all encoded data, to ensure the rightmost bits are the empty
       ones. This should only happen in the last frame of a file as all
       slices are completely filled otherwise. */
      best_slice <<= (QOA_SLICE_LEN - slice_len) * 3;
      qoa_write_u64(best_slice, bytes, &p);
    }
  }

  return p;
}

void *qoa_encode(const short *sample_data, qoa_desc *qoa, unsigned int *out_len) {
  if (
      qoa->samples == 0 ||
        qoa->samplerate == 0 || qoa->samplerate > 0xffffff ||
        qoa->channels == 0 || qoa->channels > QOA_MAX_CHANNELS
  ) {
    return NULL;
  }

  /* Calculate the encoded size and allocate */
  unsigned int num_frames = (qoa->samples + QOA_FRAME_LEN-1) / QOA_FRAME_LEN;
  unsigned int num_slices = (qoa->samples + QOA_SLICE_LEN-1) / QOA_SLICE_LEN;
  unsigned int encoded_size = 8 +                    /* 8 byte file header */
  num_frames * 8 +                               /* 8 byte frame headers */
  num_frames * QOA_LMS_LEN * 4 * qoa->channels + /* 4 * 4 bytes lms state per channel */
  num_slices * 8 * qoa->channels;                /* 8 byte slices */

  unsigned char *bytes = QOA_MALLOC(encoded_size);

  for (int c = 0; c < qoa->channels; c++) {
    /* Set the initial LMS weights to {0, 0, -1, 2}. This helps with the
     prediction of the first few ms of a file. */
    qoa->lms[c].weights[0] = 0;
    qoa->lms[c].weights[1] = 0;
    qoa->lms[c].weights[2] = -(1<<13);
    qoa->lms[c].weights[3] =  (1<<14);

    /* Explicitly set the history samples to 0, as we might have some
     garbage in there. */
    for (int i = 0; i < QOA_LMS_LEN; i++) {
      qoa->lms[c].history[i] = 0;
    }
  }


  /* Encode the header and go through all frames */
  unsigned int p = qoa_encode_header(qoa, bytes);
  #ifdef QOA_RECORD_TOTAL_ERROR
  qoa->error = 0;
  #endif

  int frame_len = QOA_FRAME_LEN;
  for (int sample_index = 0; sample_index < qoa->samples; sample_index += frame_len) {
    frame_len = qoa_clamp(QOA_FRAME_LEN, 0, qoa->samples - sample_index);
    const short *frame_samples = sample_data + sample_index * qoa->channels;
    unsigned int frame_size = qoa_encode_frame(frame_samples, qoa, frame_len, bytes + p);
    p += frame_size;
  }

  *out_len = p;
  return bytes;
}


SEXP qoaWrite_(SEXP sample_data, SEXP samplerate, SEXP sFilename){
  FILE *f = 0;
  int raw_array = 0;
  const char *fn;
  unsigned int size;
  void *encoded;


  // check type of image-input
  if (TYPEOF(sample_data) == RAWSXP) raw_array = 1;
  if (!raw_array && TYPEOF(sample_data) != INTSXP)
    Rf_error("image must be a matrix or array of raw or integer numbers");

  if (TYPEOF(sFilename) == RAWSXP) {
    f = 0;
  } else {
    if (TYPEOF(sFilename) != STRSXP || LENGTH(sFilename) < 1) Rf_error("invalid filename");
    fn = CHAR(STRING_ELT(sFilename, 0));
    f = fopen(fn, "wb");
    if (!f) Rf_error("unable to create %s", fn);
  }

  SEXP dims = Rf_getAttrib(sample_data, R_DimSymbol);
  if (dims == R_NilValue || TYPEOF(dims) != INTSXP || LENGTH(dims) < 1 || LENGTH(dims) > 8)
    Rf_error("samples must be a matrix or an array of minimum one or maximum eight channels");

  // prepare qoa_desc
  qoa_desc qoa;
  qoa.samplerate = Rf_asInteger(samplerate);
  qoa.samples = INTEGER(dims)[0];
  qoa.channels = INTEGER(dims)[1];

  // prepare incoming matrix as sample stream:
  short * sample_values = malloc(qoa.channels * qoa.samples * sizeof(short));

  if (!sample_values){
    fclose(f);
    Rf_error("Malloc error!");
  }

  // Convert matrix to single 1D array for qoa_encode
  int sampleCounter = 0;
  for (int i = 0; i < qoa.samples; i++){
    for(int j = 0; j < qoa.channels; j++){
      sample_values[sampleCounter] = (short) INTEGER(sample_data)[i+j*qoa.samples];
      sampleCounter++;
    }
  }

  encoded = qoa_encode(sample_values, &qoa, &size);
  QOA_FREE(sample_values);

  if (!encoded) {
    fclose(f);
    Rf_error("Encoding went wrong!");
    return R_NilValue;
  }

  if (f){
    fwrite(encoded, 1, size, f);
    fclose(f);
    QOA_FREE(encoded);
    return R_NilValue;
  }

  // copy encoded data into SEXP res
  SEXP res = PROTECT(allocVector(RAWSXP, size));
  unsigned char *byte = RAW(res);
  for (int i=0; i<size; i++){
    byte[i] = ((unsigned char*) encoded)[i];
  }

  UNPROTECT(1);
  QOA_FREE(encoded);
  return res;
  }
