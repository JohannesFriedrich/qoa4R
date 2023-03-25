#include <R.h>
#include <Rinternals.h>

#include <stdio.h>
#include "qoa.h"

unsigned int qoa_max_frame_size(qoa_desc *qoa) {
  return QOA_FRAME_SIZE(qoa->channels, QOA_SLICES_PER_FRAME);
}

unsigned int qoa_decode_header(const unsigned char *bytes, int size, qoa_desc *qoa) {
  unsigned int p = 0;
  if (size < QOA_MIN_FILESIZE) {
    return 0;
  }


  /* Read the file header, verify the magic number ('qoaf') and read the
   total number of samples. */
  qoa_uint64_t file_header = qoa_read_u64(bytes, &p);

  if ((file_header >> 32) != QOA_MAGIC) {
    return 0;
  }

  qoa->samples = file_header & 0xffffffff;
  if (!qoa->samples) {
    return 0;
  }

  /* Peek into the first frame header to get the number of channels and
   the samplerate. */
  qoa_uint64_t frame_header = qoa_read_u64(bytes, &p);
  qoa->channels   = (frame_header >> 56) & 0x0000ff;
  qoa->samplerate = (frame_header >> 32) & 0xffffff;

  if (qoa->channels == 0 || qoa->samples == 0 || qoa->samplerate == 0) {
    return 0;
  }

  return 8;
}

unsigned int qoa_decode_frame(const unsigned char *bytes, unsigned int size, qoa_desc *qoa, short *sample_data, unsigned int *frame_len) {
  unsigned int p = 0;
  *frame_len = 0;

  if (size < 8 + QOA_LMS_LEN * 4 * qoa->channels) {
    return 0;
  }

  /* Read and verify the frame header */
  qoa_uint64_t frame_header = qoa_read_u64(bytes, &p);
  int channels   = (frame_header >> 56) & 0x0000ff;
  int samplerate = (frame_header >> 32) & 0xffffff;
  int samples    = (frame_header >> 16) & 0x00ffff;
  int frame_size = (frame_header      ) & 0x00ffff;

  int data_size = frame_size - 8 - QOA_LMS_LEN * 4 * channels;
  int num_slices = data_size / 8;
  int max_total_samples = num_slices * QOA_SLICE_LEN;

  if (
      channels != qoa->channels ||
        samplerate != qoa->samplerate ||
        frame_size > size ||
        samples * channels > max_total_samples
  ) {
    return 0;
  }


  /* Read the LMS state: 4 x 2 bytes history, 4 x 2 bytes weights per channel */
  for (int c = 0; c < channels; c++) {
    qoa_uint64_t history = qoa_read_u64(bytes, &p);
    qoa_uint64_t weights = qoa_read_u64(bytes, &p);
    for (int i = 0; i < QOA_LMS_LEN; i++) {
      qoa->lms[c].history[i] = ((signed short)(history >> 48));
      history <<= 16;
      qoa->lms[c].weights[i] = ((signed short)(weights >> 48));
      weights <<= 16;
    }
  }


  /* Decode all slices for all channels in this frame */
  for (int sample_index = 0; sample_index < samples; sample_index += QOA_SLICE_LEN) {
    for (int c = 0; c < channels; c++) {
      qoa_uint64_t slice = qoa_read_u64(bytes, &p);

      int scalefactor = (slice >> 60) & 0xf;
      int slice_start = sample_index * channels + c;
      int slice_end = qoa_clamp(sample_index + QOA_SLICE_LEN, 0, samples) * channels + c;

      for (int si = slice_start; si < slice_end; si += channels) {
        int predicted = qoa_lms_predict(&qoa->lms[c]);
        int quantized = (slice >> 57) & 0x7;
        int dequantized = qoa_dequant_tab[scalefactor][quantized];
        int reconstructed = qoa_clamp(predicted + dequantized, -32768, 32767);

        sample_data[si] = reconstructed;
        slice <<= 3;

        qoa_lms_update(&qoa->lms[c], reconstructed, dequantized);
      }
    }
  }

  *frame_len = samples;
  return p;
}

short *qoa_decode(const unsigned char *bytes, int size, qoa_desc *qoa) {
  unsigned int p = qoa_decode_header(bytes, size, qoa);
  if (!p) {
    return NULL;
  }

  /* Calculate the required size of the sample buffer and allocate */
  int total_samples = qoa->samples * qoa->channels;
  short *sample_data = QOA_MALLOC(total_samples * sizeof(short));

  unsigned int sample_index = 0;
  unsigned int frame_len;
  unsigned int frame_size;

  /* Decode all frames */
  do {
    short *sample_ptr = sample_data + sample_index * qoa->channels;
    frame_size = qoa_decode_frame(bytes + p, size - p, qoa, sample_ptr, &frame_len);
    p += frame_size;
    sample_index += frame_len;
  } while (frame_size && sample_index < qoa->samples);

  qoa->samples = sample_index;

  return sample_data;
}

SEXP qoaRead_(SEXP sFilename) {
  const char *fn;
  void *data;
  FILE *f=0;
  qoa_desc qoa;

  int bytes_read;
  short *sample_data;

  if (TYPEOF(sFilename) != STRSXP || LENGTH(sFilename) < 1) Rf_error("invalid filename");
  fn = CHAR(STRING_ELT(sFilename, 0));
  f = fopen(fn, "rb");
  if (!f) Rf_error("unable to open %s", fn);

  // read size of bin file and open it, buffer result into data
  fseek(f, 0, SEEK_END);
  int size = ftell(f);
  if (size <= 0) {
    fclose(f);
    Rf_error("File has size 0");
  }
  fseek(f, 0, SEEK_SET);

  data = QOA_MALLOC(size);
  if (!data) {
    fclose(f);
    Rf_error("Malloc error!");
  }

  bytes_read = fread(data, 1, size, f);
  fclose(f);

  sample_data = qoa_decode(data, bytes_read, &qoa);
  QOA_FREE(data);

  if (sample_data == 0) {
    Rf_error("Decoding went wrong!");
    return R_NilValue;
  }

  // convert back to R
  SEXP res;
  res = PROTECT(allocVector(INTSXP, qoa.samples * qoa.channels));
  // // see: https://github.com/hadley/r-internals/blob/master/vectors.md#get-and-set-values
  int* samples_ = INTEGER(res);

  int sampleCounter = 0;
  for (int i = 0; i < qoa.samples; i++){
    for(int j = 0; j < qoa.channels; j++){
      samples_[i+j*qoa.samples] = sample_data[sampleCounter];
      sampleCounter++;
    }
  }

  QOA_FREE(sample_data);

  // Set dimensions for export to R
  SEXP dim;
  dim = allocVector(INTSXP, 2);
  INTEGER(dim)[0] = qoa.samples;
  INTEGER(dim)[1] = qoa.channels;
  setAttrib(res, R_DimSymbol, dim);

  SEXP list_ = PROTECT(allocVector(VECSXP, 4));

  //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  // Add members to the list
  //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  SET_VECTOR_ELT(list_, 0, res);
  SET_VECTOR_ELT(list_, 1, ScalarInteger(qoa.channels));
  SET_VECTOR_ELT(list_, 2, ScalarInteger(qoa.samplerate));
  SET_VECTOR_ELT(list_, 3, ScalarInteger(qoa.samples));

  //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  // Set the names on the list.
  //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  SEXP names = PROTECT(allocVector(STRSXP, 4));
  SET_STRING_ELT(names, 0, mkChar("data"));
  SET_STRING_ELT(names, 1, mkChar("channels"));
  SET_STRING_ELT(names, 2, mkChar("samplerate"));
  SET_STRING_ELT(names, 3, mkChar("samples"));

  setAttrib(list_, R_NamesSymbol, names);

  UNPROTECT(3);

  return list_;
}
