# coding: utf-8
lib = File.expand_path("../lib", __FILE__)
$LOAD_PATH.unshift(lib) unless $LOAD_PATH.include?(lib)
require "mini_racer_csim/version"

Gem::Specification.new do |spec|
  spec.name = "mini_racer-csim"
  spec.version = MiniRacerCsim::VERSION
  spec.authors = ["Keita Urashima", "Sam Saffron"]
  spec.email = ["ursm@ursm.jp"]

  spec.summary = "mini_racer fork with browser-fidelity ESM/realm extensions for capybara-simulated"
  spec.description =
    "A private fork of mini_racer (minimal embedded V8 for Ruby) adding " \
    "browser-level behavior used by capybara-simulated: the V8 ES Module API, " \
    "per-frame realms, realm reset, cross-process bytecode caching, an opt-in " \
    "host namespace, and a batched module-graph loader with a URL module " \
    "registry. These are niche browser-fidelity features; general users should " \
    "use upstream mini_racer. It loads under its own `mini_racer_csim` require " \
    "path and `MiniRacerCsim` namespace, so it never collides with upstream " \
    "mini_racer in the same bundle."
  spec.homepage = "https://github.com/ursm/mini_racer"
  spec.license = "MIT"

  spec.metadata = {
    "bug_tracker_uri" => "https://github.com/ursm/mini_racer/issues",
    "source_code_uri" => "https://github.com/ursm/mini_racer/tree/main"
  }

  spec.files =
    Dir[
      "lib/**/*.rb",
      "ext/**/*",
      "README.md",
      "LICENSE.txt",
      "CHANGELOG",
      "CODE_OF_CONDUCT.md"
    ]
  spec.require_paths = ["lib"]

  spec.add_development_dependency "bundler"
  spec.add_development_dependency "rake", ">= 12.3.3"
  spec.add_development_dependency "minitest", "~> 5.0"
  spec.add_development_dependency "rake-compiler"
  spec.add_development_dependency "activesupport", "> 6"
  spec.add_development_dependency "m"

  spec.add_dependency "libv8-node", MiniRacerCsim::LIBV8_NODE_VERSION
  spec.require_paths = %w[lib ext]

  spec.extensions = %w[
    ext/mini_racer_csim_loader/extconf.rb
    ext/mini_racer_csim_extension/extconf.rb
  ]

  spec.required_ruby_version = ">= 3.3"
end
