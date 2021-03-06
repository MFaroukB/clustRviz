% Generated by roxygen2: do not edit by hand
% Please edit documentation in R/data.R
\docType{data}
\name{presidential_speech}
\alias{presidential_speech}
\title{Log transformed word count of presidential speeches}
\format{A data.frame with 44 rows (one for each president) and 75 columns (log transformed word counts)}
\source{
\url{http://www.presidency.ucsb.edu}
}
\usage{
presidential_speech
}
\description{
A dataset of the top 75 most variable log-transformed word counts for
each US president aggregated over several speeches
(Inaugural, State of the Union, etc.).
Stop words have been removed and words have been stemmed.
}
\details{
Grover Cleveland was elected president twice (1892 and 1884). For our purposes his speeches are combined.
}
\keyword{datasets}
