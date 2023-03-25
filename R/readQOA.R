#' Read an QOA file
#' @param qoa_path [character] (**required**): Path to a stored qoa-file
#' @return A list with the sample data, channels, samplerate and number of samples per channel
#' If the decoding went wrong the returned value is NULL.
#' @author Johannes Friedrich
#' @examples
#' qoa_file <- system.file("extdata", "58_guitar_sarasate_stereo.qoa", package = "qoa")
#' qoa_data <- readQOA(qoa_file)
#' @md
#' @export
readQOA <- function(qoa_path) {
  qoa_data <- .Call(qoaRead_, path.expand(qoa_path))
  if (!is.null(qoa_data)){
    col_names <- c("FL", "FR", "FC", "LF", "BL", "BR", "FLC", "FRC")
    colnames(qoa_data$data) <- col_names[1:qoa_data$channels]
    return(qoa_data)
  } else {
    NULL
  }
}
