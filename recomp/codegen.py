"""Stage 2: translate every function found in stage 1 into C.

Usage: python -I codegen.py <executable> <functions.json> <out_dir> [--only 0xADDR,...]

Writes into out_dir:
  funcs.h        prototypes of all F_xxxxxxxx functions
  imports.h      prototypes of the I_name bridges the code calls (runtime, stage 3)
  code_NNN.c     the translated functions (about 40,000 instructions per file)
  dispatch.c     guest address -> function table, for calls through registers
  imports.c      C name -> Mach-O import name table
  imports_stub.c placeholder bridges (each reports the missing import), for linking now
  codegen_report.txt
"""
import json
import os
import sys
import time
from collections import Counter
from multiprocessing import Pool

from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from macho import MachO  # noqa: E402
from armc import Program, Translator, Unsupported, hx  # noqa: E402

PER_FILE = 40000

# Instructions whose translation the host replaces: address -> (declaration, C lines).  Used where a
# linker --wrap cannot reach (callers in the same generated file, where clang inlines the callee).
#   0x28d6c0 "mov r0, #0": the bloom gate, always false in the shipped game (host/games/gtracing/render/postfx.c, POSTFX)
#   0x4fde1c "mov r0, r8" in edit_text's set text 0x4fde00 (r5 = the text): NO_TOUCH_TEXT (host/games/gtracing/features/notouch.c)
#            hands it the text with "touch" / "tap" changed when Touch Screen is Off
#   0x26322c "vstr s0, [r5, #8]", the last store of the menus' GetBounds 0x2630d0 (r4 = the character, r5 =
#            the rect in screen pixels): WIDE_UI (host/games/gtracing/render/widescreen.c) puts the box where the centred stage
#            shows it
#   0x257244 "vldmia r0, {s3, s4, s5, s6}", the box load of the menus' 3D viewport fit 0x257200: WIDE_UI
#            (host/games/gtracing/render/widescreen.c gt_wide_fit) hands it a box already of the screen's shape, placed as the
#            iPad's fit places it
#   0x2632fc "add r0, sp, #8" in the menus' clip callback 0x263240 (the box at sp + 8 before its scissor):
#            WIDE_UI (gt_wide_clip) widens a clip of the stage's width to the screen's
#   0x50a854 "bl 0x50a654" in gameswf's find-by-name 0x50a81c: FAST_SWF_FIND (host/games/gtracing/features/swffind.c) walks the
#            tree natively (the same breadth-first order; GT_FAST_SWF_FIND=0 calls the original)
#   0x166254 "movw r0, #0x1e38" in the cockpit wheel's draw 0x1660f0, after the scene took its camera's
#            projection (the matrix at fp) and before the device loads it: WIDE_UI (host/games/gtracing/render/widescreen.c
#            gt_wheel_proj) enlarges the wheel with the cockpit picture
#   0x1a7588 "str r0, [r8, r4]" in GS_LoadMainMenu's loading 0x1a6c98 (r4 = 0x119c, r5 = the car table's
#            holder): the main menu's car, G+0x119c; MENU_CAR_RANDOM (host/games/gtracing/features/menu3d.c gt_menu_car, which calls the
#            game, so r0 .. r3 and sp go through c) may pick another
#   0x19c384 "vmov r1, s20" in the race camera's chase views 0x19bf38 (s20 = the heading the eye's line is
#            turned by, r8 = the camera): RACE_LOOK (host/games/gtracing/features/racelook.c gt_race_look) adds the right stick's /
#            numpad's look angle
HOST_INSNS = {
    0x19c384: ('float gt_race_look(u32 cam);', ['c->V.f[20] += gt_race_look(R8);', 'R1 = c->V.s[20];']),
    0x1a7588: ('void gt_menu_car(CPU *c, u32 holder);',
               ['CALL_PRE(0x1a758cu);', 'gt_menu_car(c, R5);', 'CALL_POST();', 'ST32(R8 + R4, R0);']),
    0x166254: ('void gt_wheel_proj(u32 m);', ['R0 = 0x1e38u;', 'gt_wheel_proj(R11);']),
    0x28d6c0: ('int gt_postfx_gate(void);', ['R0 = (u32)gt_postfx_gate();']),
    0x4fde1c: ('u32 gt_text_filter(u32 text);', ['R5 = gt_text_filter(R5);', 'R0 = R8;']),
    0x26322c: ('void gt_wide_bounds(u32 ch, u32 rect);', ['ST32(R5 + 0x8u, c->V.s[0]);', 'gt_wide_bounds(R4, R5);']),
    0x257244: ('void gt_wide_fit(u32 rect, u32 *out);', ['gt_wide_fit(R0, &c->V.s[3]);']),
    0x2632fc: ('void gt_wide_clip(u32 rect);', ['gt_wide_clip(SP + 0x8u);', 'R0 = SP + 0x8u;']),
    0x50a854: ('void gt_swf_search(CPU *c);', ['CALL_PRE(0x50a858u);', 'gt_swf_search(c);', 'CALL_POST();']),
    # FAST_SWF_FIND's cache of searches that found nothing (GT_SWF_MISS_CACHE, pass 4 item 9): emptied when a
    # character is made (gameswf's character constructor 0x4ca40c), and told each name given to a character (the six
    # tu_string assignments to a character's name at +0x44, the new name's register: 0x4cb080 the _name setter,
    # 0x4f4558.. the sprite's placing ones) so it drops the searches with that name in their path
    0x4ca410: ('extern volatile u32 gt_swf_tree_gen; extern u32 gt_swf_tree_site[8];', ['R7 = SP + 0xcu;', 'gt_swf_tree_gen++;', 'gt_swf_tree_site[0]++;']),
    0x4cb080: ('void gt_swf_named(u32 name, int site);', ['R0 = R6 + 0x44u;', 'gt_swf_named(R1, 1);']),
    0x4f4558: ('void gt_swf_named(u32 name, int site);', ['R0 = R8 + 0x44u;', 'gt_swf_named(R1, 2);']),
    0x4f5df0: ('void gt_swf_named(u32 name, int site);', ['R0 = R6 + 0x44u;', 'gt_swf_named(R8, 3);']),
    0x4f5ef4: ('void gt_swf_named(u32 name, int site);', ['R0 = R6 + 0x44u;', 'gt_swf_named(R10, 4);']),
    0x4f6190: ('void gt_swf_named(u32 name, int site);', ['R0 = R8 + 0x44u;', 'gt_swf_named(R5, 5);']),
    0x4f71f8: ('void gt_swf_named(u32 name, int site);', ['R0 = R8 + 0x44u;', 'gt_swf_named(R10, 6);']),
}

_g = {}


def _init(exe, fjson):
    m = MachO(exe)
    data = json.load(open(fjson))
    _g['prog'] = Program(m, data)
    md = {}
    for thumb in (False, True):
        d = Cs(CS_ARCH_ARM, CS_MODE_THUMB if thumb else CS_MODE_ARM)
        d.detail = True
        md[thumb] = d
    _g['md'] = md


def emit_function(prog, fn, md):
    """C text of one function, plus (errors, warnings, unsupported mnemonics)."""
    tr = Translator(prog, fn)
    start, end = fn['start'], fn['end']
    body = []
    errors, unsup = [], Counter()
    addrs = set()
    for a, e in fn['runs']:
        term, last_end = True, a
        for ins in md[fn['thumb']].disasm(prog.m.read(a, e - a), a):
            addrs.add(ins.address)
            try:
                lines, term = tr.translate(ins)
            except Unsupported as x:
                errors.append('%x %s %s: %s' % (ins.address, ins.mnemonic, ins.op_str, x))
                unsup[ins.mnemonic.split('.')[0]] += 1
                lines, term = ['TRAP(%s, TRAP_UDF); /* not translated */' % hx(ins.address)], True
            if ins.address in HOST_INSNS:
                lines = HOST_INSNS[ins.address][1]
            body.append((ins.address, '%s %s' % (ins.mnemonic, ins.op_str), lines))
            last_end = ins.address + ins.size
        if last_end != e:
            errors.append('%x: run decodes to %x, expected %x' % (a, last_end, e))
        if not term:
            if last_end == end and end in prog.func_thumb:
                lines = tr.j_exit() + ['CALL_PRE(LR);', 'F_%x(c);' % end, 'return;']
                body.append((None, 'falls through into the next function', lines))
            else:
                body.append((None, 'end of code', ['TRAP(%s, TRAP_FALLOFF);' % hx(last_end)]))
    taken = [t for t in fn.get('taken', []) if t in addrs]
    labels = set(tr.labels) | set(taken)
    missing = sorted(t for t in labels if t not in addrs)
    for t in missing:
        errors.append('%x: jump target %x is not an instruction' % (start, t))
    name = fn.get('name')
    out = ['/* %s %s */' % ('Thumb' if fn['thumb'] else 'ARM', name or '')] if name else []
    out += [HOST_INSNS[a][0] for a in sorted(addrs) if a in HOST_INSNS]
    out.append('void F_%x(CPU *c) {' % start)
    out.append('  LOCALS_IN();')
    if tr.has_sjlj:
        out.append('  SJLJ_FRAME();')
        out.append('  if (SJLJ_SETJMP()) {')
        out.append('    RELOAD_ALL();')
        out.append('    switch (c->resume_pc & ~1u) {')
        for t in taken:
            out.append('    case %s: goto L_%x;' % (hx(t), t))
        out.append('    }')
        out.append('    TRAP(%s, TRAP_RESUME);' % hx(start))
        out.append('  }')
    for addr, text, lines in body:
        if addr is not None and addr in labels:
            out.append('L_%x:; LABEL_HOOK();' % addr)
        out.append('  /* %s %s */' % ('%x' % addr if addr is not None else '--', text.replace('*/', '* /')))
        out.extend('  ' + x for x in lines)
    for t in missing:
        out.append('L_%x: TRAP(%s, TRAP_FALLOFF);' % (t, hx(t)))
    out.append('}')
    return '\n'.join(out) + '\n', errors, tr.warn, unsup


def _work(job):
    idx, fns = job
    prog, md = _g['prog'], _g['md']
    texts, errors, warns, unsup = [], [], [], Counter()
    for fn in fns:
        t, e, w, u = emit_function(prog, fn, md)
        texts.append(t)
        errors += e
        warns += w
        unsup += u
    return idx, texts, errors, warns, unsup, dict(prog.imports_used)


HEADER = '#include "arm_rt.h"\n#include "funcs.h"\n#include "imports.h"\n\n'


def main():
    exe, fjson, out = sys.argv[1], sys.argv[2], sys.argv[3]
    only = None
    if '--only' in sys.argv:
        only = {int(x, 16) for x in sys.argv[sys.argv.index('--only') + 1].split(',')}
    os.makedirs(out, exist_ok=True)
    t0 = time.time()
    data = json.load(open(fjson))
    funcs = data['functions']
    if only:
        funcs = [f for f in funcs if f['start'] in only]
    # Split into files of about PER_FILE instructions (by code size / 3).
    jobs, cur, n = [], [], 0
    for f in funcs:
        cur.append(f)
        n += sum(e - a for a, e in f['runs']) // 3
        if n >= PER_FILE:
            jobs.append((len(jobs), cur))
            cur, n = [], 0
    if cur:
        jobs.append((len(jobs), cur))
    results = []
    if len(jobs) == 1:
        _init(exe, fjson)
        results = [_work(jobs[0])]
    else:
        with Pool(min(os.cpu_count() or 4, 12), initializer=_init, initargs=(exe, fjson)) as pool:
            results = pool.map(_work, jobs, chunksize=1)
    errors, warns, unsup, imports = [], [], Counter(), {}
    for idx, texts, e, w, u, imp in sorted(results):
        with open(os.path.join(out, 'code_%03d.c' % idx), 'w', newline='\n') as fp:
            fp.write(HEADER)
            fp.writelines(texts)
        errors += e
        warns += w
        unsup += u
        imports.update(imp)
    with open(os.path.join(out, 'funcs.h'), 'w', newline='\n') as fp:
        fp.write('/* generated by recomp/codegen.py */\n#pragma once\n')
        for f in data['functions']:
            fp.write('void F_%x(CPU *c);\n' % f['start'])
    with open(os.path.join(out, 'imports.h'), 'w', newline='\n') as fp:
        fp.write('/* generated by recomp/codegen.py: bridges provided by the runtime */\n#pragma once\n')
        for c in sorted(imports):
            fp.write('void %s(CPU *c);\n' % c)
    with open(os.path.join(out, 'imports.c'), 'w', newline='\n') as fp:
        fp.write(HEADER + 'typedef struct { const char *name; void (*fn)(CPU *); } ImportEntry;\n')
        fp.write('const ImportEntry rt_imports[] = {\n')
        for c in sorted(imports):
            fp.write('  {"%s", %s},\n' % (imports[c], c))
        fp.write('};\nconst unsigned rt_import_count = %d;\n' % len(imports))
    with open(os.path.join(out, 'imports_stub.c'), 'w', newline='\n') as fp:
        fp.write(HEADER + 'void rt_missing_import(CPU *c, const char *name);\n')
        for c in sorted(imports):
            fp.write('void %s(CPU *c) { rt_missing_import(c, "%s"); }\n' % (c, imports[c]))
    with open(os.path.join(out, 'dispatch.c'), 'w', newline='\n') as fp:
        fp.write(HEADER + 'typedef struct { u32 addr; void (*fn)(CPU *); } DispatchEntry;\n')
        fp.write('/* addr has bit 0 set for Thumb functions; sorted by address */\n')
        fp.write('const DispatchEntry rt_dispatch[] = {\n')
        for f in data['functions']:
            fp.write('  {0x%x, F_%x},\n' % (f['start'] | int(f['thumb']), f['start']))
        fp.write('};\nconst unsigned rt_dispatch_count = %d;\n' % len(data['functions']))
    lines = ['Stage 2 code generation report  (%s)' % time.strftime('%Y-%m-%d %H:%M'),
             'functions %d, files %d, imports used %d, time %.1f s' % (
                 len(funcs), len(jobs), len(imports), time.time() - t0),
             'untranslated instructions %d' % sum(unsup.values()),
             '  ' + ', '.join('%s %d' % kv for kv in unsup.most_common()), '',
             'errors (%d):' % len(errors)] + ['  ' + e for e in errors[:400]] + \
            ['', 'warnings (%d):' % len(warns)] + ['  ' + w for w in warns[:400]]
    with open(os.path.join(out, 'codegen_report.txt'), 'w') as fp:
        fp.write('\n'.join(lines) + '\n')
    print('\n'.join(lines[:6]))


if __name__ == '__main__':
    main()
