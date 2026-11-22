#!/usr/bin/env python3
"""Translate the game's HLSL compile requests to Metal Shading Language.

Input: a folder of requests captured by dcrun (DCUSA_SHADERS=<dir> writes
<dir>/src/<key>.hlsl and <key>.json the first time D3DCompile sees one). The key
is a hash of the source, the macros, the entry point and the profile.

Output, in <dir>/msl/:
  <key>.metal  MSL 2.1 source, entry point main0
  <key>.json   what D3DReflect reports for it: textures, samplers and constant
               buffers with their slots, and each buffer's members and offsets
  <key>.refl   the same, one resource per line, for the runtime
  failed.json  requests that did not translate, with the reason

Resource slots in the MSL are the SPIR-V bindings (--msl-decoration-binding),
and the reflection reports the same numbers, so the game binds a texture to the
slot the Metal function reads. Constant buffer offsets come from the same
SPIR-V, so the bytes the game writes land where the MSL reads them.

A file <dir>/override/<key>.metal (with a matching .json) replaces the
translation of that one request. Overrides are written to match behaviour; the
original HLSL never leaves the player's machine.

Tools: glslang (HLSL front end, `brew install glslang`) and spirv-cross.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

STAGES = {'vs': 'vert', 'ps': 'frag', 'gs': 'geom', 'cs': 'comp', 'hs': 'tesc', 'ds': 'tese'}
# D3D_SHADER_INPUT_TYPE and D3D_SRV_DIMENSION, as D3DReflect reports them.
SIT_CBUFFER, SIT_TEXTURE, SIT_SAMPLER = 0, 2, 3
DIMENSIONS = {'texture1D': 2, 'texture2D': 4, 'texture2DArray': 5, 'texture2DMS': 6, 'texture3D': 8,
              'textureCube': 9, 'textureCubeArray': 10}


def run(cmd):
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return p.returncode, p.stdout


ARRAY_DECL = re.compile(r'(\w+)\s+(\w+)\s*\[\s*(\d+)\s*\]\s*=\s*INITARRAY(\d)\s*\(\s*\w+\s*,([^;]*)\)\s*;')
TEXTURE_TYPES = {'Texture2D', 'Texture2DShadow', 'TextureCube', 'Texture2DMS', 'Texture3D'}


def split_args(text):
    """Top-level comma split of a call's argument text."""
    out, depth, cur = [], 0, ''
    for ch in text:
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    out.append(cur.strip())
    return out


def call_end(src, start):
    """Index just past the ')' that closes the '(' at src[start]."""
    depth = 0
    for i in range(start, len(src)):
        if src[i] == '(':
            depth += 1
        elif src[i] == ')':
            depth -= 1
            if depth == 0:
                return i + 1
    return -1


def unroll_texture_arrays(src):
    """A local array of textures built from globals, e.g.
         Texture2DShadow TexShadow[4] = INITARRAY4(Texture2DShadow, a, b, c, d);
         ... tex2DCmpOfs(TexShadow[index], s, uv, off) ...
    becomes a selection between whole calls on each global:
         (index == 0 ? tex2DCmpOfs(a, s, uv, off) : index == 1 ? ... : tex2DCmpOfs(d, ...))
    Texture reads have no side effects and a ?: is not control flow, so the
    value is the same. glslang cannot legalize the array form (g_PreShadowPS:
    it emits one SPIR-V id twice). Returns the new source and the number of
    calls rewritten."""
    arrays = {}
    def drop(m):
        if m.group(1) not in TEXTURE_TYPES:
            return m.group(0)
        arrays[m.group(2)] = split_args(m.group(5))
        return '/* %s %s: unrolled at each use */' % (m.group(1), m.group(2))
    src = ARRAY_DECL.sub(drop, src)
    rewritten = 0
    for name, items in arrays.items():
        use = re.compile(r'(\w+)\s*\(\s*' + re.escape(name) + r'\s*\[')
        pos = 0
        while True:
            m = use.search(src, pos)
            if not m:
                break
            open_paren = src.index('(', m.start())
            end = call_end(src, open_paren)
            args = split_args(src[open_paren + 1:end - 1])
            first = args[0]
            index = first[first.index('[') + 1:first.rindex(']')].strip()
            rest = ', '.join(args[1:])
            calls = ['%s(%s%s)' % (m.group(1), item, ', ' + rest if rest else '') for item in items]
            expr = calls[-1]
            for i in range(len(items) - 2, -1, -1):
                expr = '((%s) == %d ? %s : %s)' % (index, i, calls[i], expr)
            src = src[:m.start()] + expr + src[end:]
            pos = m.start() + len(expr)
            rewritten += 1
        if re.search(r'\b' + re.escape(name) + r'\b(?!: unrolled)', src.replace('%s: unrolled' % name, '')):
            raise ValueError('texture array %s is used other than as a first call argument' % name)
    return src, rewritten


def compile_spirv(hlsl, stage, spv, tmp):
    """The first of these whose SPIR-V passes spirv-val: glslang as is; with
    local texture arrays unrolled (unroll_texture_arrays); without glslang's
    optimizer (-Od)."""
    base = ['glslang', '-V', '-D', '-e', 'main', '-S', stage, '--auto-map-bindings', '--hlsl-offsets',
            '-fhlsl_functionality1', '-o', spv]
    log = ''
    text = open(hlsl, encoding='latin-1').read()
    attempts = [('default', [], hlsl)]
    try:
        unrolled_text, n = unroll_texture_arrays(text)
        if n:
            unrolled = os.path.join(tmp, os.path.basename(hlsl))
            with open(unrolled, 'w', encoding='latin-1') as f:
                f.write(unrolled_text)
            attempts.append(('unrolled-texture-arrays', [], unrolled))
    except ValueError as e:
        log += str(e) + '\n'
    attempts.append(('-Od', ['-Od'], hlsl))
    for label, extra, path in attempts:
        rc, out = run(base[:2] + extra + base[2:] + [path])
        log += out
        if rc != 0:
            continue
        rc, out = run(['spirv-val', spv])
        log += out
        if rc == 0:
            return label, log
    return None, log


def reflect(spv):
    rc, out = run(['spirv-cross', spv, '--reflect'])
    if rc != 0:
        raise RuntimeError(out)
    return json.loads(out)


REGISTER = re.compile(r'(\w+)\s*:\s*register\s*\(\s*([bst])(\d+)\s*\)')


def explicit_registers(hlsl, stage):
    """{name: number} for resources the source pins with register(): the game
    binds some constant buffers at those numbers itself (CB_PER_SCENE at b9),
    so they have to stay. The macros that write them are expanded first."""
    rc, out = run(['glslang', '-E', '-D', '-S', stage, hlsl])
    return {m.group(1): int(m.group(3)) for m in REGISTER.finditer(out)} if rc == 0 else {}


def renumber_bindings(spv, refl, explicit):
    """Slots the way D3D assigns them: textures (t#), samplers (s#) and constant
    buffers (b#) are numbered per class; a resource with an explicit register
    keeps it, and the others take the lowest free slots in declaration order.
    glslang's --auto-map-bindings gave every resource a distinct binding in
    declaration order; the OpDecorate Binding words are rewritten in place."""
    import struct
    remap = {}
    for group in ('separate_images', 'separate_samplers', 'ubos'):
        resources = sorted(refl.get(group, []), key=lambda r: r.get('binding', 0))
        taken = {explicit[r['name']] for r in resources if r['name'] in explicit}
        free = (n for n in range(128) if n not in taken)
        for r in resources:
            remap[r.get('binding', 0)] = explicit[r['name']] if r['name'] in explicit else next(free)
    data = bytearray(open(spv, 'rb').read())
    words = list(struct.unpack('<%dI' % (len(data) // 4), data))
    i = 5
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 0xFFFF
        if count == 0:
            break
        if op == 71 and count >= 4 and words[i + 2] == 33 and words[i + 3] in remap:  # OpDecorate .. Binding
            words[i + 3] = remap[words[i + 3]]
        i += count
    open(spv, 'wb').write(struct.pack('<%dI' % len(words), *words))


SCALAR = {'float': 4, 'int': 4, 'uint': 4, 'bool': 4}


def member_size(m):
    """Bytes of a scalar, vector or matrix member, as D3D reports a variable's
    size (a float3 is 12, a float4x4 64). None for arrays and structs."""
    if m.get('array') or m['type'].startswith('_'):
        return None
    t = m['type']
    for prefix, scalar in (('ivec', 'int'), ('uvec', 'uint'), ('bvec', 'bool'), ('vec', 'float')):
        if t.startswith(prefix) and t[len(prefix):].isdigit():
            return SCALAR[scalar] * int(t[len(prefix):])
    if t.startswith('mat'):
        dims = t[3:].split('x')
        if all(d.isdigit() for d in dims):
            cols = int(dims[0])
            rows = int(dims[1]) if len(dims) > 1 else cols
            return 4 * cols * rows
    return SCALAR.get(t)


# Stage interface locations by semantic, so a vertex shader and a pixel shader
# translated on their own still link the way D3D links them (by semantic, not by
# declaration order). SV_ semantics are builtins and have no location.
SEMANTIC_BASE = {'TEXCOORD': (0, 16), 'COLOR': (16, 4), 'NORMAL': (20, 2), 'TANGENT': (22, 1), 'BINORMAL': (23, 1),
                 'POSITION': (24, 1), 'BLENDWEIGHT': (25, 1), 'BLENDINDICES': (26, 1), 'PSIZE': (27, 1),
                 'FOG': (28, 1)}


def split_semantic(sem):
    m = re.match(r'^(.*?)(\d*)$', sem.upper())
    return m.group(1), int(m.group(2) or 0)


def semantic_location(sem):
    name, index = split_semantic(sem)
    if name in SEMANTIC_BASE:
        base, count = SEMANTIC_BASE[name]
        if index < count:
            return base + index
    raise ValueError('no interface location for semantic ' + sem)


def spirv_words(spv):
    import struct
    data = open(spv, 'rb').read()
    return list(struct.unpack('<%dI' % (len(data) // 4), data))


def spirv_string(words, at):
    raw = b''.join(w.to_bytes(4, 'little') for w in words[at:])
    return raw[:raw.index(b'\0')].decode('latin-1')


def interface(spv, stage):
    """Rewrites stage-interface locations to semantic_location(), except vertex
    inputs (those are the input layout's attributes) and pixel outputs
    (SV_Target n is color n). Returns the vertex inputs as
    [(location, semantic name, semantic index)]."""
    import struct
    words = spirv_words(spv)
    semantic, storage, location_at = {}, {}, {}
    i = 5
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 0xFFFF
        if count == 0:
            break
        if op in (5632, 5633) and words[i + 2] == 5635:  # OpDecorateString(GOOGLE) .. UserSemantic
            semantic[words[i + 1]] = spirv_string(words, i + 3)
        elif op == 71 and words[i + 2] == 30:  # OpDecorate .. Location
            location_at[words[i + 1]] = i + 3
        elif op == 59:  # OpVariable type id storage
            storage[words[i + 2]] = words[i + 3]
        i += count
    inputs = []
    for var, at in location_at.items():
        sc = storage.get(var)
        sem = semantic.get(var)
        if sem is None or sem.upper().startswith('SV_'):
            continue
        if stage == 'vert' and sc == 1:
            name, index = split_semantic(sem)
            inputs.append((words[at], name, index))
            continue
        if stage == 'frag' and sc == 3:
            continue
        if sc in (1, 3):
            words[at] = semantic_location(sem)
    open(spv, 'wb').write(struct.pack('<%dI' % len(words), *words))
    return sorted(inputs)


def describe(refl, key, stage, options):
    types = refl.get('types', {})
    textures = [{'name': r['name'], 'slot': r.get('binding', 0),
                 'dimension': DIMENSIONS.get(r.get('type', 'texture2D'), 4)}
                for r in refl.get('separate_images', [])]
    samplers = [{'name': r['name'], 'slot': r.get('binding', 0), 'comparison': r.get('type') == 'samplerShadow'}
                for r in refl.get('separate_samplers', [])]
    cbuffers = []
    for u in refl.get('ubos', []):
        t = types.get(u['type'], {})
        members = []
        mlist = t.get('members', [])
        for i, m in enumerate(mlist):
            end = mlist[i + 1]['offset'] if i + 1 < len(mlist) else u.get('block_size', m['offset'])
            size = member_size(m)
            if size is None:
                size = end - m['offset']  # arrays and structs: up to the next member
            elements = 1
            for n in m.get('array', []):
                elements *= n
            # SPIR-V's row_major is HLSL's column_major (the default): D3D
            # reports that as D3D_SVC_MATRIX_COLUMNS.
            major = 'c' if m.get('row_major') else ('r' if m['type'].startswith('mat') else '-')
            members.append({'name': m['name'], 'offset': m['offset'], 'size': size, 'type': m['type'],
                            'elements': elements if m.get('array') else 0, 'major': major})
        name = u['name']
        if name in ('$Global', '_Global'):
            name = '$Globals'
        # D3D buffer sizes are multiples of 16, and Metal rounds the struct the
        # same way; the game sizes the buffer it creates from this number.
        size = (u.get('block_size', 0) + 15) // 16 * 16
        cbuffers.append({'name': name, 'slot': u.get('binding', 0), 'size': size,
                         'members': members})
    return {'key': key, 'stage': stage, 'entry': 'main0', 'msl': key + '.metal', 'options': options,
            'textures': sorted(textures, key=lambda r: r['slot']),
            'samplers': sorted(samplers, key=lambda r: r['slot']),
            'cbuffers': sorted(cbuffers, key=lambda r: r['slot'])}


def translate_one(src_dir, out_dir, key, tmp):
    meta = json.load(open(os.path.join(src_dir, key + '.json')))
    profile = meta['target']
    stage = STAGES.get(profile[:2])
    if stage is None:
        return 'unknown profile ' + profile
    spv = os.path.join(tmp, key + '.spv')
    options, log = compile_spirv(os.path.join(src_dir, key + '.hlsl'), stage, spv, tmp)
    if options is None:
        return 'glslang: ' + ' '.join(l for l in log.splitlines() if 'ERROR' in l or 'error' in l)[:300]
    renumber_bindings(spv, reflect(spv), explicit_registers(os.path.join(src_dir, key + '.hlsl'), stage))
    try:
        inputs = interface(spv, stage)
    except ValueError as e:
        return str(e)
    msl = os.path.join(out_dir, key + '.metal')
    rc, out = run(['spirv-cross', '--msl', '--msl-version', '20100', '--msl-decoration-binding', spv,
                   '--output', msl])
    if rc != 0:
        return 'spirv-cross: ' + out.strip()[:300]
    info = describe(reflect(spv), key, stage, options)
    info['profile'] = profile
    info['inputs'] = [{'location': l, 'semantic': n, 'index': x} for l, n, x in inputs]
    info['name'] = meta.get('name', '')
    with open(os.path.join(out_dir, key + '.json'), 'w') as f:
        json.dump(info, f, indent=1)
    write_refl(os.path.join(out_dir, key + '.refl'), info)
    return None


def write_refl(path, info):
    """The line format src/recomp/d3d11.cpp reads (no JSON parser there)."""
    lines = ['stage %s' % info['stage']]
    lines += ['input %d %s %d' % (i['location'], i['semantic'], i['index']) for i in info.get('inputs', [])]
    lines += ['texture %d %d %s' % (t['slot'], t['dimension'], t['name']) for t in info['textures']]
    lines += ['sampler %d %d %s' % (t['slot'], 1 if t['comparison'] else 0, t['name']) for t in info['samplers']]
    for cb in info['cbuffers']:
        lines.append('cbuffer %d %d %s' % (cb['slot'], cb['size'], cb['name']))
        lines += ['member %d %d %s %d %s %s' % (m['offset'], m['size'], m['type'], m.get('elements', 0), m.get('major', '-'),
                                                m['name']) for m in cb['members']]
    with open(path, 'w') as f:
        f.write('\n'.join(lines) + '\n')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dir', help='folder with src/ (captured requests); msl/ is written')
    ap.add_argument('--only', help='translate just this request key (the runtime does this on a miss)')
    ap.add_argument('--quiet', action='store_true')
    args = ap.parse_args()
    for tool in ('glslang', 'spirv-cross'):
        if shutil.which(tool) is None:
            sys.exit('%s is not installed (brew install glslang spirv-cross)' % tool)
    src_dir = os.path.join(args.dir, 'src')
    out_dir = os.path.join(args.dir, 'msl')
    over_dir = os.path.join(args.dir, 'override')
    os.makedirs(out_dir, exist_ok=True)
    keys = sorted(f[:-5] for f in os.listdir(src_dir) if f.endswith('.json'))
    if args.only:
        keys = [k for k in keys if k == args.only]
    failed = {}
    overridden = 0
    with tempfile.TemporaryDirectory() as tmp:
        for key in keys:
            o_msl, o_json = os.path.join(over_dir, key + '.metal'), os.path.join(over_dir, key + '.json')
            if os.path.exists(o_msl) and os.path.exists(o_json):
                shutil.copyfile(o_msl, os.path.join(out_dir, key + '.metal'))
                shutil.copyfile(o_json, os.path.join(out_dir, key + '.json'))
                write_refl(os.path.join(out_dir, key + '.refl'), json.load(open(o_json)))
                overridden += 1
                continue
            why = translate_one(src_dir, out_dir, key, tmp)
            if why:
                failed[key] = why
    if not args.only:
        with open(os.path.join(out_dir, 'failed.json'), 'w') as f:
            json.dump(failed, f, indent=1)
    if args.quiet:
        return 1 if failed else 0
    print('%d requests: %d translated, %d overridden, %d failed' % (
        len(keys), len(keys) - len(failed) - overridden, overridden, len(failed)))
    for key, why in sorted(failed.items()):
        print('  %s  %s' % (key, why))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
