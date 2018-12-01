project('VMAF', 'cpp',
  default_options : ['buildtype=release', 'b_ndebug=if-release', 'cpp_std=c++14'],
  meson_version : '>=0.48.0',
  version : '2'
)

add_project_arguments('-ffast-math', language : 'cpp')

sources = [
  'VMAF/VMAF.cpp'
]

vapoursynth_dep = dependency('vapoursynth').partial_dependency(compile_args : true, includes : true)

libvmaf_dep = dependency('libvmaf')

if host_machine.cpu_family().startswith('x86')
  add_project_arguments('-mfpmath=sse', '-msse2', language : 'cpp')
endif

vapoursynth_plugindir = join_paths(vapoursynth_dep.get_pkgconfig_variable('libdir'), 'vapoursynth')

shared_module('vmaf', sources,
  dependencies : [vapoursynth_dep, libvmaf_dep],
  install : true,
  install_dir : vapoursynth_plugindir,
  gnu_symbol_visibility : 'hidden'
)

install_subdir('VMAF/model',
  install_dir : vapoursynth_plugindir
)
