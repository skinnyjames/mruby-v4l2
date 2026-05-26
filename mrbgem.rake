MRuby::Gem::Specification.new('mruby-v4l2') do |spec|
  spec.license = 'MIT'
  spec.author  = 'skinnyjames'
  spec.summary = 'mruby video 4 linux 2 bindings'
  spec.version = '0.1.0'
  spec.add_dependency 'mruby-sleep', github: 'matsumotory/mruby-sleep'
end
