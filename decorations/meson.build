deco_sdk = declare_dependency(dependencies: [pixman, tllist, fcft])

decorations = []
foreach deco : ['background', 'border', 'stack', 'underline', 'overline']
  if plugs_as_libs
    shared_module('@0@'.format(deco), '@0@.c'.format(deco),
                  dependencies: deco_sdk,
                  name_prefix: 'decoration_',
                  install: true,
                  install_dir: join_paths(get_option('libdir'), 'yambar'))
  else
    decorations += [declare_dependency(
      sources: '@0@.c'.format(deco),
      dependencies: deco_sdk,
      compile_args: '-DHAVE_PLUGIN_@0@'.format(deco.underscorify()))]
  endif
endforeach
