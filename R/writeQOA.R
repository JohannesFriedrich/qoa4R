#' Write an QOA file
#' @param samples [matrix] (**required**): audio file represented by a integer matrix or array.
#' @param samplerate [integer] (**required**): samplerate of the data given in argument 'samples'.
#' @param target [character] or [connections] or [raw]: Either name of the file to write, a binary connection or a raw vector (raw() - the default - is good enough) indicating that the output should be a raw vector.
#' @return The result is either stored in a file (if target is a file name), in a raw vector (if target is a raw vector) or sent to a binary connection.
#' @author Johannes Friedrich
#' @examples
#' ## (1) Write to raw() -> see bytes
#' wav <- writeQOA(wav_example$data, wav_example$samplerate)
#' rawToChar(head(wav,4)) ##qoaf
#'
#' \dontrun{
#' ## (2) Write to a *.qoi file
#' writeQOA(wav_example$data, wav_example$samplerate, "wav_to_qoa.qoa")
#' }
#' @md
#' @export
writeQOA <- function(samples, samplerate, target = raw()) {
  if (inherits(target, "connection")) {
    r <- .Call(qoaWrite_, samples, samplerate, raw())
    writeBin(r, target)
    invisible(NULL)
  } else invisible(.Call(qoaWrite_, samples, samplerate, if (is.raw(target)) target else path.expand(target)))
}
