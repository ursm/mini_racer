require 'mkmf'

extension_name = 'mini_racer_csim_loader'
dir_config extension_name

$CXXFLAGS += " -fvisibility=hidden "

create_makefile extension_name
