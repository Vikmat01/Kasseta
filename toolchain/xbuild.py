#!/usr/bin/env python3
"""Cross-build for TrimUI tg5040 (aarch64 glibc) with clang+lld and generated stub libs.

usage: xbuild.py OUT SRC... [-I dir] [-D def]
Links dynamically against libc.so.6/libm.so.6/libpthread.so.0/libdl.so.2/libSDL2-2.0.so.0
present on the device. All glibc symbols are bound to GLIBC_2.17 (aarch64 baseline).
"""
import os, subprocess, sys

TC = os.path.dirname(os.path.abspath(__file__))
SYSROOT = os.environ.get('SYSROOT', os.path.join(os.path.dirname(TC), 'third_party', 'sysroot'))
RES = subprocess.check_output(['clang', '-print-resource-dir'], text=True).strip()
CFLAGS = ['--target=aarch64-linux-gnu', '-nostdinc', '-isystem', RES + '/include',
          '-isystem', SYSROOT + '/usr/include', '-D__GLIBC_MINOR__=28', '-fPIE',
          '-fno-stack-protector', '-mno-outline-atomics', '-U_FORTIFY_SOURCE',
          '-mcpu=cortex-a53', '-O2', '-ffunction-sections', '-fdata-sections',
          '-D_GNU_SOURCE', '-DPLATFORM_TG5040']
X86 = bool(os.environ.get('XB_X86'))  # self-test mode: same stub technique against the host glibc
TARGET = [] if X86 else ['--target=aarch64-linux-gnu']
VER = 'GLIBC_2.2.5' if X86 else 'GLIBC_2.17'
DYNLINK = '/lib64/ld-linux-x86-64.so.2' if X86 else '/lib/ld-linux-aarch64.so.1'
CRT = 'crt_start_x86.S' if X86 else 'crt_start.S'
if X86:
    CFLAGS = ['-O2', '-fPIE', '-fno-stack-protector', '-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=0']

LIBM = set('''acos asin atan atan2 cos sin tan cosh sinh tanh exp exp2 expm1 log log10 log2 log1p pow sqrt
cbrt hypot ceil floor round trunc fmod lround lrint rint nearbyint ldexp frexp modf fmin fmax copysign
acosf asinf atanf atan2f cosf sinf tanf coshf sinhf tanhf expf exp2f logf log10f log2f powf sqrtf ceilf
floorf roundf truncf fmodf lroundf ldexpf frexpf modff fminf fmaxf hypotf cbrtf copysignf sincos sincosf
fabs fabsf llround llroundf'''.split())
LIBPTHREAD_PREFIX = ('pthread_', 'sem_')
LIBDL = {'dlopen', 'dlsym', 'dlclose', 'dlerror'}
DATA = {'stdin': 8, 'stdout': 8, 'stderr': 8, 'environ': 8, '__environ': 8, 'optarg': 8, 'optind': 4}
# glibc symbols that do NOT exist at GLIBC_2.17 and must not be used
FORBIDDEN = {'reallocarray', 'getrandom', 'getentropy', 'explicit_bzero', 'stat', 'fstat', 'lstat',
             'stat64', 'fstat64', 'lstat64', 'fstatat', 'mknod', '__isoc23_strtol', '__isoc23_sscanf',
             'fcntl64', 'memfd_create', 'copy_file_range', 'statx'}


_hostver = {}
def host_version(lib, sym):
    if lib not in _hostver:
        m = {}
        path = '/lib/x86_64-linux-gnu/' + lib
        for line in subprocess.check_output(['objdump', '-T', path], text=True).splitlines():
            p = line.split()
            if len(p) >= 7 and p[-2].startswith('GLIBC_'):
                v, n = p[-2], p[-1]
                if '(' in v: continue
                if n not in m or (not line.count('(') and v > m[n]): m[n] = v
        _hostver[lib] = m
    if sym == '__libc_start_main': return 'GLIBC_2.2.5'  # old-style entry that calls our init
    return _hostver[lib].get(sym) or _hostver.get('libc.so.6', {}).get(sym) or 'GLIBC_2.2.5'


def run(cmd):
    r = subprocess.run(cmd)
    if r.returncode:
        sys.exit('FAILED: ' + ' '.join(cmd))


def main():
    args = sys.argv[1:]
    out = args.pop(0)
    srcs, extra = [], []
    while args:
        a = args.pop(0)
        if a in ('-I', '-D'):
            extra += [a + args.pop(0)]
        elif a.startswith('-'):
            extra.append(a)
        else:
            srcs.append(a)
    bdir = out + '.build'
    os.makedirs(bdir, exist_ok=True)
    objs = []
    for s in srcs + [TC + '/crt_init.c', TC + '/' + CRT]:
        o = os.path.join(bdir, os.path.basename(s) + '.o')
        if not os.path.exists(o) or os.path.getmtime(o) < max(os.path.getmtime(s), max(
                (os.path.getmtime(os.path.join(d, f)) for d in {os.path.dirname(s) or '.'}
                 for f in os.listdir(d) if f.endswith('.h')), default=0)):
            run(['clang'] + CFLAGS + extra + ['-c', s, '-o', o])
        objs.append(o)
    und = set()
    defined = set()
    for line in subprocess.check_output(['llvm-nm', '-P'] + objs, text=True).splitlines():
        p = line.split()
        if len(p) < 2 or p[0].endswith(':'):
            continue
        if p[1] in 'Uw':
            und.add(p[0])
        else:
            defined.add(p[0])
    und -= defined
    und -= {'__init_array_start', '__init_array_end', '_DYNAMIC', '_GLOBAL_OFFSET_TABLE_'}
    bad = und & FORBIDDEN
    if bad:
        sys.exit('forbidden glibc symbols: %s' % sorted(bad))
    libs = {'libc.so.6': [], 'libm.so.6': [], 'libpthread.so.0': [], 'libdl.so.2': [],
            'libSDL2-2.0.so.0': []}
    for s in sorted(und):
        if s.startswith('SDL_'):
            libs['libSDL2-2.0.so.0'].append(s)
        elif s in LIBM:
            libs['libm.so.6'].append(s)
        elif s.startswith(LIBPTHREAD_PREFIX) and not X86:
            libs['libpthread.so.0'].append(s)
        elif s in LIBDL:
            libs['libdl.so.2'].append(s)
        elif s.startswith('__aarch64_') or s.startswith('__') and s.endswith('tf3'):
            sys.exit('compiler runtime symbol needed: ' + s)
        else:
            libs['libc.so.6'].append(s)
    stubdir = os.path.join(bdir, 'stubs')
    os.makedirs(stubdir, exist_ok=True)
    stubs = []
    for lib, syms in libs.items():
        if not syms and lib not in ('libc.so.6',):
            continue
        if lib == 'libc.so.6' and '__libc_start_main' not in syms:
            syms.append('__libc_start_main')
        c = os.path.join(stubdir, lib + '.c')
        with open(c, 'w') as f:
            for s in syms:
                if s in DATA:
                    f.write('char %s[%d];\n' % (s, DATA[s]))
                else:
                    f.write('void %s(void){}\n' % s)
        so = os.path.join(stubdir, lib)
        cmd = ['clang', '-w'] + TARGET + ['-nostdlib', '-shared', '-fPIC', '-fuse-ld=lld',
               '-Wl,-soname,' + lib, c, '-o', so]
        if lib != 'libSDL2-2.0.so.0':
            vs = os.path.join(stubdir, lib + '.map')
            groups = {}
            for sy in syms:
                groups.setdefault(host_version(lib, sy) if X86 else VER, []).append(sy)
            with open(vs, 'w') as f:
                for v, ss in sorted(groups.items()):
                    f.write('%s { global: %s; };\n' % (v, '; '.join(ss)))
            cmd.insert(-2, '-Wl,--version-script=' + vs)
        run(cmd)
        stubs.append(so)
    run(['clang'] + TARGET + ['-nostdlib', '-fuse-ld=lld', '-pie',
         '-Wl,--dynamic-linker=' + DYNLINK, '-Wl,--gc-sections', '-Wl,-z,relro',
         '-Wl,--hash-style=both', '-Wl,--as-needed', '-o', out] + objs + stubs + ['-s'])
    for lib, syms in libs.items():
        if syms:
            print('%-18s %d symbols' % (lib, len(syms)))


if __name__ == '__main__':
    main()
