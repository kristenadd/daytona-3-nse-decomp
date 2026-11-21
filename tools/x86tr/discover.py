#!/usr/bin/env python3
"""Finds every function of a prepared Daytona.exe image.

There is no .pdata in a 32-bit PE, so functions come from the union of:
  entry       the image entry point
  call        direct call targets (found while walking)
  tail        direct jumps into another function
  vtable      RTTI-described virtual function tables
  seh         the SafeSEH handler table (load config)
  eh          C++ EH funclets named by FuncInfo (unwind actions, catch blocks)
  data        pointers into .text from .rdata / .data that land outside
              every known body and jump table (initializer tables, function
              pointers, m2e dispatch tables, C SEH scope tables)

    PYTHONPATH=build/pylib python3 tools/x86tr/discover.py build/mac-host/prepared/DCUSA2019.cache build/x86tr/2019
"""
import argparse
import collections
import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from image import Image  # noqa: E402
from cfg import CONDITIONAL as CONDITIONAL_MNEMS, HEX, Walker  # noqa: E402

EH_MAGICS = (0x19930520, 0x19930521, 0x19930522)
NORETURN_IMPORTS = {'ExitProcess', 'RaiseException', 'FatalAppExitA', 'FatalAppExitW', 'ExitThread'}


def rtti_vtables(img):
    """{vtable va: [entries]} for every vtable whose COL points at a valid TypeDescriptor."""
    rdata = [s for s in img.sections if s.name == '.rdata'][0]
    words = struct.unpack_from('<%dI' % (rdata.file_backed // 4), img.data, rdata.va - img.base)
    by_value = collections.defaultdict(list)
    for i, w in enumerate(words):
        if rdata.va <= w < rdata.va + rdata.file_backed:
            by_value[w].append(rdata.va + 4 * i)
    vtables = {}
    for col in list(by_value):
        if col % 4 or not img.contains(col, 20):
            continue
        sig, offset, cd, td, chd = struct.unpack_from('<IIIII', img.data, col - img.base)
        if sig != 0 or not img.contains(td, 12) or not img.contains(chd, 16):
            continue
        name = img.bytes(td + 8, 4)
        if name not in (b'.?AV', b'.?AU'):
            continue
        for loc in by_value[col]:
            vt = loc + 4
            entries = []
            while img.contains(vt + 4 * len(entries), 4):
                t = img.u32(vt + 4 * len(entries))
                if not img.in_text(t):
                    break
                entries.append(t)
            if entries:
                vtables[vt] = entries
    return vtables


def seh_handlers(img):
    lc = img.load_config()
    if not lc or not lc['se_handler_table']:
        return []
    return [img.base + img.u32(lc['se_handler_table'] + 4 * i) for i in range(lc['se_handler_count'])]


def eh_funclets(img, walker, handlers):
    """Unwind actions and catch handlers of every FuncInfo reached from an EH handler thunk."""
    out = set()
    funcinfos = set()
    for h in handlers:
        va = h
        for _ in range(8):
            insn = walker.dec.at(va)
            if insn is None:
                break
            if insn.mnem == 'mov' and insn.ops.startswith('eax, 0x'):
                fi = int(insn.ops[5:], 16)
                if img.contains(fi, 28) and img.u32(fi) in EH_MAGICS:
                    funcinfos.add(fi)
            if insn.mnem in ('jmp', 'ret'):
                break
            va = insn.next
    for fi in funcinfos:
        magic, max_state, unwind, ntry, trymap = struct.unpack_from('<IiIiI', img.data, fi - img.base)
        for s in range(max(0, min(max_state, 100000))):
            if img.contains(unwind + 8 * s, 8):
                action = img.u32(unwind + 8 * s + 4)
                if action and img.in_text(action):
                    out.add(action)
        for t in range(max(0, min(ntry, 10000))):
            tb = trymap + 20 * t
            if not img.contains(tb, 20):
                break
            ncatch, handlers_va = img.i32(tb + 12), img.u32(tb + 16)
            for c in range(max(0, min(ncatch, 1000))):
                hv = handlers_va + 16 * c
                if img.contains(hv, 16):
                    addr = img.u32(hv + 12)
                    if img.in_text(addr):
                        out.add(addr)
    return out, funcinfos


IMPOSSIBLE = {'enter', 'in', 'out', 'insb', 'insd', 'insw', 'outsb', 'outsd', 'outsw', 'arpl', 'bound', 'les', 'lds',
              'hlt', 'into', 'aaa', 'aas', 'daa', 'das', 'aam', 'aad', 'salc', 'retf', 'iretd', 'cli', 'sti', 'int',
              'int1', 'lcall', 'ljmp', 'wait', 'fwait', 'clts', 'invd', 'wbinvd', 'rdmsr', 'wrmsr', 'sysenter', 'lgdt',
              'sgdt', 'lidt', 'sidt', 'lldt', 'sldt', 'ltr', 'str', 'lmsw', 'smsw', 'xlatb', 'icebp', 'cmc'}
IMPOSSIBLE_FORMS = {'ds', 'es', 'ss', 'cs', 'fs', 'gs'}  # push / pop of segment registers


def utf16_text(w):
    """Two UTF-16 code units of printable ASCII (0x00XX00YY): string data, not a pointer."""
    return (w & 0xFF00FF00) == 0 and 0x20 <= (w & 0xFF) < 0x7F and 0x20 <= (w >> 16) < 0x7F


def code_word(img, w):
    return img.in_text(w) and not utf16_text(w)


def data_pointers(img):
    """Code addresses stored in data, and the subset that sits in runs of
    two or more consecutive code pointers (function tables)."""
    out = collections.Counter()
    tables = set()
    for s in img.sections:
        if s.executable or s.name in ('.rsrc',):
            continue
        n = s.file_backed // 4
        words = struct.unpack_from('<%dI' % n, img.data, s.va - img.base)
        for i, w in enumerate(words):
            if img.in_text(w) and not utf16_text(w):
                out[w] += 1
                if (i and code_word(img, words[i - 1])) or (i + 1 < n and code_word(img, words[i + 1])):
                    tables.add(w)
    return out, tables


class Discovery:
    def __init__(self, img):
        self.img = img
        self.walker = Walker(img)
        self.kinds = collections.defaultdict(set)   # entry -> {root kinds}
        self.functions = {}
        self.rejected = collections.Counter()
        text = img.text
        self.text_va = text.va
        self.cov = bytearray(text.file_backed)      # 1 where a walked instruction lies
        self.starts = bytearray(text.file_backed)   # 1 where a walked instruction starts

    def add(self, va, kind):
        if not self.img.in_text(va):
            return
        self.kinds[va].add(kind)
        self.walker.function_starts.add(va)

    def record(self, fn):
        t = self.text_va
        for insn in fn.insns.values():
            self.starts[insn.va - t] = 1
            self.cov[insn.va - t:insn.next - t] = b'\x01' * insn.size

    def recount(self):
        self.cov = bytearray(len(self.cov))
        self.starts = bytearray(len(self.starts))
        for fn in self.functions.values():
            self.record(fn)

    def covered(self, va):
        return self.cov[va - self.text_va] != 0

    def fixpoint(self):
        pending = [va for va in self.kinds if va not in self.functions]
        while pending:
            va = pending.pop()
            if va in self.functions:
                continue
            fn = self.walker.walk(va)
            self.functions[va] = fn
            self.record(fn)
            for t in fn.callees:
                if self.img.in_text(t) and t not in self.kinds:
                    self.add(t, 'call')
                    pending.append(t)
                elif self.img.in_text(t):
                    self.kinds[t].add('call')
            for t in fn.tail_calls:
                if t not in self.kinds:
                    self.add(t, 'tail')
                    pending.append(t)

    def rewalk(self):
        self.functions = {va: self.walker.walk(va) for va in self.kinds}
        self.recount()

    def immediates(self):
        """Code addresses used as immediate operands (push offset fn, mov reg, offset fn)."""
        out = set()
        for fn in self.functions.values():
            for insn in fn.insns.values():
                if insn.mnem in ('call', 'jmp') or insn.mnem in CONDITIONAL_MNEMS or '0x' not in insn.ops:
                    continue
                for h in HEX.findall(insn.ops):
                    v = int(h, 16)
                    if self.img.in_text(v):
                        out.add(v)
        return out

    def after_boundary(self, va):
        """Preceded by alignment padding or the end of a ret: where MSVC starts functions."""
        img = self.img
        if not img.in_text(va - 1):
            return True
        if img.u8(va - 1) in (0xCC, 0x90):
            return True
        return img.u8(va - 1) == 0xC3 or (img.in_text(va - 3) and img.u8(va - 3) == 0xC2)

    def sane(self, fn):
        """Plausible MSVC user-mode code that doesn't cut into known instructions."""
        if fn.undecodable or not fn.insns:
            return False
        t = self.text_va
        for insn in fn.insns.values():
            first = insn.mnem.split()[-1]
            if first in IMPOSSIBLE or insn.ops in IMPOSSIBLE_FORMS:
                return False
            if not self.starts[insn.va - t] and self.cov[insn.va - t]:
                return False  # starts inside a known instruction
            if any(self.starts[insn.va + 1 - t:insn.next - t]):
                return False  # a known instruction starts inside this one
        return True

    def weak_round(self, candidates, kind, tables=None):
        added = 0
        for va in sorted(candidates):
            if va in self.kinds or self.covered(va) or va in self.walker.data_in_text:
                continue
            if self.walker.dec.at(va) is None:
                continue
            if not (self.after_boundary(va) or (tables is not None and va in tables)):
                continue
            fn = self.walker.walk(va)
            if not self.sane(fn):
                self.rejected[kind] += 1
                continue
            self.add(va, kind)
            self.functions[va] = fn
            self.record(fn)
            added += 1
        if added:
            self.fixpoint()
        return added

    def gap_starts(self):
        """Uncovered 16-aligned code after padding or a ret that nothing points to."""
        text = self.img.text
        first = (text.va + 15) & ~15
        return [va for va in range(first, text.va + text.file_backed, 16)
                if not self.covered(va) and va not in self.walker.data_in_text and self.after_boundary(va)
                and self.img.u8(va) not in (0xCC, 0x90, 0x00)]

    def demote_interior(self):
        """Weak roots that lie inside another function's instructions are labels, not functions."""
        inside = set()
        for va, fn in self.functions.items():
            inside.update(a for a in fn.insns if a != va)
        interior = [va for va, ks in self.kinds.items() if va in inside and ks <= {'data', 'imm', 'gap'}]
        for va in interior:
            del self.kinds[va]
            self.walker.function_starts.discard(va)
            self.functions.pop(va, None)
        return interior

    def mark_noreturn(self, imports):
        for slot, (dll, name) in imports.items():
            if name in NORETURN_IMPORTS:
                self.walker.noreturn.add(slot)
        changed = True
        while changed:
            changed = False
            for va, fn in self.functions.items():
                if va in self.walker.noreturn:
                    continue
                returns = fn.indirect_jumps or fn.jump_tables or any(
                    i.mnem.startswith('ret') for i in fn.insns.values()) or any(
                    t not in self.walker.noreturn for t in fn.tail_calls)
                if not returns and fn.insns:
                    self.walker.noreturn.add(va)
                    changed = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cache')
    ap.add_argument('out')
    args = ap.parse_args()
    t0 = time.time()
    img = Image(args.cache)
    d = Discovery(img)
    imports = img.imports()

    d.add(img.entry, 'entry')
    vtables = rtti_vtables(img)
    for entries in vtables.values():
        for t in entries:
            d.add(t, 'vtable')
    handlers = seh_handlers(img)
    for h in handlers:
        d.add(h, 'seh')
    funclets, funcinfos = eh_funclets(img, d.walker, handlers)
    for f in funclets:
        d.add(f, 'eh')
    d.fixpoint()
    print('strong roots: %d functions after fixpoint (%.0f s)' % (len(d.functions), time.time() - t0))

    # Weak roots, outside every body and table at an instruction start of their
    # own: code immediates first (they come from code already known to run), then data pointers.
    pointers, tables = data_pointers(img)
    for rounds in range(12):
        added = d.weak_round(d.immediates(), 'imm')
        if added:
            print('imm round %d: +%d roots, %d functions' % (rounds + 1, added, len(d.functions)))
            continue
        added = d.weak_round(pointers, 'data', tables)
        if added:
            print('data round %d: +%d roots, %d functions' % (rounds + 1, added, len(d.functions)))
            continue
        added = d.weak_round(d.gap_starts(), 'gap')
        if not added:
            break
        print('gap round %d: +%d roots, %d functions' % (rounds + 1, added, len(d.functions)))
    print('weak roots rejected as implausible code:', dict(d.rejected))

    d.mark_noreturn(imports)
    d.rewalk()  # bodies with every start and no-return callee known
    interior = d.demote_interior()
    d.rewalk()
    d.mark_noreturn(imports)
    d.rewalk()
    print('%d weak roots were interior labels' % len(interior))

    text = img.text
    owners = bytearray(text.file_backed)
    for fn in d.functions.values():
        for insn in fn.insns.values():
            for b in range(insn.va - text.va, insn.next - text.va):
                if owners[b] < 255:
                    owners[b] += 1
    covered = {text.va + i for i, c in enumerate(owners) if c}
    shared_bytes = sum(1 for c in owners if c > 1)
    gap_code = 0
    runs = []
    run_start = None
    for i in range(text.file_backed + 1):
        va = text.va + i
        gap = i < text.file_backed and not owners[i] and va not in d.walker.data_in_text and img.u8(va) not in (0xCC, 0x90)
        if gap:
            gap_code += 1
            if run_start is None:
                run_start = va
        elif run_start is not None:
            runs.append((run_start, va - run_start))
            run_start = None
    kinds = collections.Counter(k for ks in d.kinds.values() for k in ks)
    only = collections.Counter(next(iter(ks)) for ks in d.kinds.values() if len(ks) == 1)
    stats = {
        'functions': len(d.functions),
        'instructions': sum(len(f.insns) for f in d.functions.values()),
        'covered_bytes': len(covered),
        'text_bytes': text.file_backed,
        'jump_table_bytes': len(d.walker.data_in_text),
        'uncovered_non_padding_bytes': gap_code,
        'uncovered_runs_16_plus': sum(1 for r in runs if r[1] >= 16),
        'bytes_in_more_than_one_function': shared_bytes,
        'roots_by_kind': dict(kinds),
        'functions_found_only_by': dict(only),
        'vtables': len(vtables),
        'seh_handlers': len(handlers),
        'funcinfos': len(funcinfos),
        'jump_tables': sum(len(f.jump_tables) for f in d.functions.values()),
        'unbounded_jump_tables': sum(1 for f in d.functions.values() for j in f.jump_tables.values() if not j.bounded),
        'unresolved_indirect_jumps': sum(len(f.indirect_jumps) for f in d.functions.values()),
        'undecodable_in_bodies': sum(len(f.undecodable) for f in d.functions.values()),
        'noreturn_functions': len([v for v in d.walker.noreturn if v in d.functions]),
        'seconds': round(time.time() - t0, 1),
    }
    # Reachability: from every root except the padding sweep, through calls,
    # tail calls and code addresses used as immediates by reachable code.
    imms = {}
    for va, fn in d.functions.items():
        refs = set()
        for insn in fn.insns.values():
            if insn.mnem in ('call', 'jmp') or insn.mnem in CONDITIONAL_MNEMS or '0x' not in insn.ops:
                continue
            refs.update(v for v in (int(h, 16) for h in HEX.findall(insn.ops)) if v in d.functions)
        imms[va] = refs
    reachable = set()
    stack = [va for va, ks in d.kinds.items() if ks & {'entry', 'vtable', 'seh', 'eh', 'data'} and va in d.functions]
    while stack:
        va = stack.pop()
        if va in reachable:
            continue
        reachable.add(va)
        fn = d.functions[va]
        for t in list(fn.callees) + list(fn.tail_calls) + list(imms[va]):
            if t in d.functions and t not in reachable:
                stack.append(t)
    stats['reachable_functions'] = len(reachable)
    stats['reachable_instructions'] = sum(len(d.functions[v].insns) for v in reachable)
    print('reachable: %d functions, %d instructions' % (len(reachable), stats['reachable_instructions']))

    os.makedirs(args.out, exist_ok=True)
    funcs = []
    for va in sorted(d.functions):
        fn = d.functions[va]
        funcs.append({
            'entry': va, 'size': fn.size, 'insns': len(fn.insns), 'kinds': sorted(d.kinds[va]),
            'noreturn': va in d.walker.noreturn,
            'reachable': va in reachable,
            'imms': sorted(imms[va]),
            'callees': sorted(fn.callees), 'tail_calls': sorted(fn.tail_calls),
            'jump_tables': {str(k): {'table': j.va, 'targets': j.targets, 'bounded': j.bounded,
                                     'index_table': j.index_table} for k, j in fn.jump_tables.items()},
            'indirect_jumps': fn.indirect_jumps, 'undecodable': fn.undecodable,
        })
    with open(os.path.join(args.out, 'uncovered.txt'), 'w') as f:
        for va, n in sorted(runs, key=lambda r: -r[1]):
            if n >= 16:
                f.write('%08x %d\n' % (va, n))
    with open(os.path.join(args.out, 'functions.json'), 'w') as f:
        json.dump({'edition': img.edition, 'stats': stats, 'noreturn_imports': sorted(
            s for s in d.walker.noreturn if s not in d.functions), 'functions': funcs}, f)
    print(json.dumps(stats, indent=1))


if __name__ == '__main__':
    main()
