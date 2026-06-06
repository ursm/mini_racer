require "mini_racer_csim/version"
require "pathname"

module MiniRacerCsim
  class Binary
    attr_reader :data

    def initialize(data)
      raise TypeError, "wrong argument type #{data.class} (expected String)" unless data.is_a?(String)
      @data = data
    end
  end
end

# mini_racer-csim is a hard fork of mini_racer with its own require path and
# `MiniRacerCsim` namespace (and `mini_racer_csim_*` C extensions), so it never
# collides with — and loads deterministically alongside — upstream mini_racer.
# V8 (CRuby C extension) only; the TruffleRuby backend was dropped.
if ENV["LD_PRELOAD"].to_s.include?("malloc")
  require "mini_racer_csim_extension"
else
  require "mini_racer_csim_loader"
  ext_filename = "mini_racer_csim_extension.#{RbConfig::CONFIG["DLEXT"]}"
  # Resolve the extension from the gem's own require_paths, falling back to the
  # default lib/ext layout so it is found however we're loaded.
  spec = Gem.loaded_specs["mini_racer-csim"]
  ext_path =
    (spec ? spec.require_paths : %w[lib ext]).map do |p|
      (p = Pathname.new(p)).absolute? ? p : Pathname.new(__dir__).parent + p
    end
  ext_found = ext_path.map { |p| p + ext_filename }.find { |p| p.file? }

  unless ext_found
    raise LoadError,
          "Could not find #{ext_filename} in #{ext_path.map(&:to_s)}"
  end
  MiniRacerCsim::Loader.load(ext_found.to_s)
end

require "thread"
require "json"
require "io/wait"

module MiniRacerCsim
  class Error < ::StandardError; end

  class ContextDisposedError < Error; end
  class PlatformAlreadyInitialized < Error; end

  class EvalError < Error; end
  class ParseError < EvalError; end
  class ScriptTerminatedError < EvalError; end
  class V8OutOfMemoryError < EvalError; end

  class RuntimeError < EvalError
    def initialize(message)
      message, *@frames = message.split("\n")
      @frames.map! { "JavaScript #{_1.strip}" }
      super(message)
    end

    def backtrace
      frames = super
      @frames + frames unless frames.nil?
    end
  end

  class ScriptError < EvalError
    def initialize(message)
      message, *@frames = message.split("\n")
      @frames.map! { "JavaScript #{_1.strip}" }
      super(message)
    end

    def backtrace
      frames = super || []
      @frames + frames
    end
  end

  class SnapshotError < Error
    def initialize(message)
      message, *@frames = message.split("\n")
      @frames.map! { "JavaScript #{_1.strip}" }
      super(message)
    end

    def backtrace
      frames = super
      @frames + frames unless frames.nil?
    end
  end

  class Context
    def load(filename)
      eval(File.read(filename))
    end

    def write_heap_snapshot(file_or_io)
      f = nil
      implicit = false

      if String === file_or_io
        f = File.open(file_or_io, "w")
        implicit = true
      else
        f = file_or_io
      end

      raise ArgumentError, "file_or_io" unless File === f

      f.write(heap_snapshot())
    ensure
      f.close if implicit
    end
  end
end
