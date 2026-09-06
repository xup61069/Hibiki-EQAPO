# User-supplied impulse responses

The public source tree and installer do not bundle impulse-response audio.
At runtime, place only files that you are licensed to use in an `IRs`
directory beside the active Hibiki EQAPO configuration file. The Convolution
editor discovers supported audio files there recursively. This repository
directory is an ignored staging location and is not installed or read at
runtime.

Do not commit third-party impulse responses here unless their exact license,
attribution, redistribution terms, and source revision have been reviewed.
