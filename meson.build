project('VMAF', 'cpp',
  default_options: ['buildtype=release', 'warning_level=2', 'b_lto=true', 'b_ndebug=if-release', 'cpp_std=c++17'],
  license: 'MIT',
  meson_version: '>=0.51.0',
  version: '6'
)

cxx = meson.get_compiler('cpp')

gcc_syntax = cxx.get_argument_syntax() == 'gcc'

if get_option('buildtype') == 'release'
  add_project_arguments(gcc_syntax ? ['-fno-math-errno', '-fno-trapping-math'] : ['/GS-', '/fp:except-'], language: 'cpp')
endif

if gcc_syntax
  vapoursynth_dep = dependency('vapoursynth').partial_dependency(compile_args: true, includes: true)
  libvmaf_dep = dependency('libvmaf', version: '>=2.0.0')
  deps = [vapoursynth_dep, libvmaf_dep]
  install_dir = vapoursynth_dep.get_variable(pkgconfig: 'libdir') / 'vapoursynth'
else
  libvmaf_dep = cxx.find_library('libvmaf')
  thread_dep = cxx.find_library('pthreads')
  deps = [libvmaf_dep, thread_dep]
  install_dir = get_option('libdir') / 'vapoursynth'
endif

sources = [
  'VMAF/VMAF.cpp'
]

if host_machine.cpu_family().startswith('x86') and gcc_syntax
  add_project_arguments('-mfpmath=sse', '-msse2', language: 'cpp')
endif

shared_module('vmaf', sources,
  dependencies: deps,
  install: true,
  install_dir: install_dir,
  gnu_symbol_visibility: 'hidden'
)
