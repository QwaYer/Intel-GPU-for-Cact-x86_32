# ==============================================================================
# Intel GPU (i915 family, Gen9.5 / Coffee Lake "UHD Graphics 630") — Meson build
# ==============================================================================
#
#   meson setup build-meson --cross-file cross/i686-cact-clang.ini \
#       -Dkern_root=../CactKernel-x86_32 -Dlocal_repo=../LocalRepoCactOS-x86_32
#   ninja -C build-meson          # build-meson/i915.cctk
#   ninja -C build-meson stage    # copy into <local_repo>/lib
#
# The .cctk is a single relocatable object (ET_REL) that the kernel's PCI module
# loader maps and relocates, so every source is compiled to an i386 object and
# the objects are combined with `clang -r` (same recipe as the rt2800usb and
# other multi-file modules).
#
# The driver is a display/KMS driver only: it binds the Intel VGA function,
# brings up the display power wells, reads EDID over GMBUS/DDC (or DP AUX) and
# registers the DRM device with the discovered connectors and modes.  The render
# engine (GuC/HuC, execbuf) is out of scope.
# ==============================================================================

project('i915-mod', 'c',
  meson_version: '>=1.0.0',
  default_options: [
    'c_std=gnu17',
    'warning_level=1',
    'buildtype=plain',
  ])

fs = import('fs')
bash = find_program('bash')
cc = meson.get_compiler('c')

src_root = meson.project_source_root()

# Path options are relative to this repo's root unless absolute.
_kern = get_option('kern_root')
kern_root = _kern.startswith('/') ? _kern : src_root / _kern
_local = get_option('local_repo')
local_repo = _local.startswith('/') ? _local : src_root / _local
install_dir = local_repo / 'lib'

# Kernel headers this module compiles against.
mod_includes = [
  kern_root / 'Cact/kernel/sync',
  kern_root / 'Cact/kernel/core',
  kern_root / 'Cact/drivers/pci/enum',
  kern_root / 'Cact/drivers/pci',
  kern_root / 'Cact/drivers/pci/msi',
  # DRM/KMS core ABI (drm_drv.h) plus the vendored uapi headers it includes.
  kern_root / 'Cact/drivers/gpu/drm',
  kern_root / 'Cact/drivers/gpu/drm/uapi',
]

mod_c_args = [
  '-ffreestanding',
  '-fno-pie',
  '-fno-stack-protector',
  '-nostdlib',
  # Ring 0: the kernel has not enabled SSE for itself, so clang must not lower
  # struct copies to SSE2 (the same trap the kernel build hits).
  '-mno-mmx',
  '-mno-sse',
  '-mno-sse2',
  '-O2',
]
foreach d : mod_includes
  mod_c_args += '-I' + d
endforeach
mod_c_args += '-I' + src_root

_cc_cmd = ' '.join(cc.cmd_array())
_args   = ' '.join(mod_c_args)

_sources = ['i915_mod.c', 'intel_display.c', 'intel_gmbus.c', 'intel_edid.c',
            'intel_modeset.c', 'intel_irq.c', 'intel_cursor.c']

i915_cctk = custom_target('i915.cctk',
  input: _sources,
  output: 'i915.cctk',
  # Headers are not compiled, so list them as depend_files: a header-only edit
  # must still rebuild the module.
  depend_files: ['intel_i915.h', 'intel_regs.h', 'intel_edid.h'],
  command: [bash, '-c', (
    'set -e; ' +
    'out="$1"; shift; ' +
    'd=$(mktemp -d); ' +
    'for f in "$@"; do ' +
      _cc_cmd + ' -m32 ' + _args + ' -c "$f" -o "$d/$(basename "$f" .c).o"; ' +
    'done; ' +
    _cc_cmd + ' -m32 -nostdlib -r "$d"/*.o -o "$out"; ' +
    'rm -rf "$d"'),
    'bash', '@OUTPUT@', '@INPUT@'],
  build_by_default: true)

# Meson reserves the name "install" for itself, hence "stage".
run_target('stage',
  command: [bash, '-c', 'set -e; mkdir -p "$1"; cp -f "$2" "$1/i915.cctk"',
            'bash', install_dir, i915_cctk])
