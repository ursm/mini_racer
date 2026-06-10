require 'mkmf'

$srcs = ["mini_racer_csim_extension.c", "mini_racer_v8.cc"]

require_relative '../../lib/mini_racer_csim/version'
gem 'libv8-node', MiniRacerCsim::LIBV8_NODE_VERSION
require 'libv8-node'

IS_DARWIN = RUBY_PLATFORM =~ /darwin/

have_library('pthread')
have_library('objc') if IS_DARWIN
$CXXFLAGS += " -Wall" unless $CXXFLAGS.split.include? "-Wall"
$CXXFLAGS += " -g" unless $CXXFLAGS.split.include? "-g"
$CXXFLAGS += " -rdynamic" unless $CXXFLAGS.split.include? "-rdynamic"
$CXXFLAGS += " -fPIC" unless $CXXFLAGS.split.include? "-rdynamic" or IS_DARWIN
$CXXFLAGS += " -std=c++20"
$CXXFLAGS += " -fpermissive"
$CXXFLAGS += " -fno-rtti"
$CXXFLAGS += " -fno-exceptions"
$CXXFLAGS += " -fno-strict-aliasing"
#$CXXFLAGS += " -DV8_COMPRESS_POINTERS"
# Hide internal symbols from the dynamic symbol table on BOTH translation
# units. mini_racer_csim_extension.c (which #includes serde.c) defines the
# internal C-ABI shared with mini_racer_v8.cc (v8_dispatch/v8_reply/v8_roundtrip/
# v8_thread_main/v8_get_flags/single_threaded/des). Those names are identical to
# upstream mini_racer's; if both gems are dlopen'd RTLD_GLOBAL (the LD_PRELOAD
# ~= malloc path in lib/mini_racer_csim.rb) the second-loaded library would
# cross-bind them to the first's definitions over a divergent Context layout and
# hang/corrupt. Hiding them keeps intra-.so linkage intact while removing the
# collision. Init_mini_racer_csim_extension stays exported via its explicit
# visibility("default") attribute.
$CXXFLAGS += " -fvisibility=hidden "
$CFLAGS += " -fvisibility=hidden "

# __declspec gets used by clang via ruby 3.x headers...
$CXXFLAGS += " -fms-extensions"

$CXXFLAGS += " -Wno-reserved-user-defined-literal" if IS_DARWIN

if IS_DARWIN
  $LDFLAGS.insert(0, " -stdlib=libc++ ")
else
  $LDFLAGS.insert(0, " -lstdc++ ")
end

# check for missing symbols at link time
# $LDFLAGS += " -Wl,--no-undefined " unless IS_DARWIN
# $LDFLAGS += " -Wl,-undefined,error " if IS_DARWIN

if ENV['CXX']
  puts "SETTING CXX"
  CONFIG['CXX'] = ENV['CXX']
end

CONFIG['LDSHARED'] = '$(CXX) -shared' unless IS_DARWIN
if CONFIG['warnflags']
  CONFIG['warnflags'].gsub!('-Wdeclaration-after-statement', '')
  CONFIG['warnflags'].gsub!('-Wimplicit-function-declaration', '')
end

if enable_config('debug') || enable_config('asan')
  CONFIG['debugflags'] << ' -ggdb3 -O0'
end

Libv8::Node.configure_makefile

# --exclude-libs is only for i386 PE and ELF targeted ports
append_ldflags("-Wl,--exclude-libs=ALL ")

if enable_config('asan')
  $CXXFLAGS.insert(0, " -fsanitize=address ")
  $LDFLAGS.insert(0, " -fsanitize=address ")
end

# there doesn't seem to be a CPP macro for this in Ruby 2.6:
if RUBY_ENGINE == 'ruby'
  $CPPFLAGS += ' -DENGINE_IS_CRUBY '
end

create_makefile 'mini_racer_csim_extension'
