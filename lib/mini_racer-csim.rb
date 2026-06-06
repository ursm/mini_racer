# This gem is published as `mini_racer-csim`; its implementation lives under the
# `mini_racer_csim` require path (and the `MiniRacerCsim` namespace). This shim
# lets `require 'mini_racer-csim'` — e.g. Bundler's autorequire for the gem name
# — load the library.
require 'mini_racer_csim'
