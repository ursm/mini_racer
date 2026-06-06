# This gem is published as `mini_racer-csim`, but its implementation lives in
# `mini_racer` (it is a drop-in fork). This shim lets `require 'mini_racer-csim'`
# — e.g. Bundler's autorequire for the gem name — load the library.
require 'mini_racer'
