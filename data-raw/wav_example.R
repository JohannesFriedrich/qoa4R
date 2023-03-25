## code to prepare `DATASET` dataset goes here

wav_original <- tuneR::readWave("/Users/johannes/GIT/qoa/qoa_test_samples_2023_02_18/sqam/58_guitar_sarasate_stereo.wav", toWaveMC = TRUE)

wav_example <- list(
  data = wav_original@.Data,
  samplerate = wav_original@samp.rate,
  samples = nrow(wav_original@.Data),
  channels = ncol(wav_original@.Data)
)
usethis::use_data(wav_example,overwrite = TRUE)
