require 'mkmf'

http_parser_dir = File.expand_path('../vendor/http-parser', __FILE__)
$CFLAGS << " -I#{http_parser_dir} "
$CFLAGS << "-ggdb3 -O0 -Wall -Wextra" if ENV['DEBUG']

have_func("strnstr")
dir_config("ruby_http_parser")
create_makefile("ruby_http_parser")
