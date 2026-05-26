MRuby::Build.new do |conf|
  toolchain :clang

  conf.gem __dir__
  conf.gembox "stdlib"
  conf.gembox "stdlib-io"
  # Generate mrbc command
  conf.gem :core => "mruby-bin-mirb"
  conf.gem :core => "mruby-bin-mruby"
end