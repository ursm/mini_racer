# frozen_string_literal: true

module MiniRacerCsim
  # mini_racer-csim fork: upstream version + a fork revision segment.
  # 0.21.1.0 = first fork release on upstream 0.21.1; bump the 4th segment for
  # fork-only changes, reset it when rebasing onto a new upstream version.
  VERSION = "0.21.1.3"
  LIBV8_NODE_VERSION = "~> 24.12.0.1"
end
