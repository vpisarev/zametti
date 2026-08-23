#!/usr/bin/env python3
# ИНВЕНТАРИЗАЦИЯ КОДА: что в программе не используется. См. README.md рядом.
#
# Три прохода одним clangd (свой индекс, см. lsp.py):
#   closure — замыкание живого в zametti-core от корней app/ и store/ → closure.json
#   macros  — поля ZM_SETTING* (settings.h, app_state.h): текст как фильтр + goToDefinition → macros.json
#   locals  — функции/методы во всех наших .cpp/.h (app, store, tests, core) с числом ссылок → locals.json
import sys, os, glob, re, json, collections, time
# Корень репозитория — на два каталога выше scripts/inventory.
os.chdir(os.environ.get('ZAMETTI_ROOT',
                        os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lsp import Clangd, SCR
modes = sys.argv[1:] or ['closure', 'macros', 'locals']
c = Clangd(jobs=4)
c.wait_index(max_s=5400)
KINDS = {5: 'class', 23: 'struct', 10: 'enum', 12: 'func', 6: 'method', 9: 'ctor', 14: 'const', 13: 'var', 26: 'alias', 2: 'ns'}
WANT = {'class', 'struct', 'enum', 'func', 'method', 'ctor', 'const', 'var', 'alias'}
log = lambda *a: print(*a, file=sys.stderr, flush=True)

def src_lines(f, cache={}):
    if f not in cache: cache[f] = open(f, encoding='utf-8', errors='replace').read().split('\n')
    return cache[f]

# ------------------------------------------------------------------ closure
if 'closure' in modes:
    t0 = time.time()
    files = sorted(glob.glob('zametti-core/*/*.h') + glob.glob('zametti-core/*/*.cpp'))
    nodes = []
    def walk(syms, f, path, parent):
        for s in syms:
            rng = s['range']; sel = s['selectionRange']['start']
            nid = len(nodes)
            nodes.append({'id': nid, 'file': f, 'name': '::'.join(path + (s['name'],)), 'kind': KINDS.get(s['kind'], str(s['kind'])),
                          'line': sel['line'], 'col': sel['character'],
                          'start': (rng['start']['line'], rng['start']['character']), 'end': (rng['end']['line'], rng['end']['character']),
                          'parent': parent})
            walk(s.get('children', []), f, path + (s['name'],), nid)
    for f in files:
        walk(c.symbols(f), f, (), None); c.close(f)
    log('closure: nodes', len(nodes), round(time.time() - t0), 's')
    per_file = collections.defaultdict(list)
    for n in nodes: per_file[n['file']].append(n)
    def enclosing(f, line, col):
        best = None
        for n in per_file.get(f, []):
            if n['kind'] == 'ns': continue
            if n['start'] <= (line, col) <= n['end'] and (best is None or (n['start'] >= best['start'] and n['end'] <= best['end'])): best = n
        return best
    edges_in = collections.defaultdict(set); root_refs = collections.defaultdict(list); test_refs = collections.defaultdict(list)
    other_refs = collections.defaultdict(list)
    cur = None
    for n in nodes:
        if n['kind'] not in WANT: continue
        if cur != n['file']:
            if cur: c.close(cur)
            c.open(n['file']); cur = n['file']
        for r in c.refs(n['file'], n['line'], n['col']):
            f = c.rel(r['uri']); L = r['range']['start']['line']; C = r['range']['start']['character']
            if f.startswith(('app/', 'store/')): root_refs[n['id']].append(f'{f}:{L+1}')
            elif f.startswith('tests/'): test_refs[n['id']].append(f'{f}:{L+1}')
            elif f.startswith('zametti-core/'):
                e = enclosing(f, L, C)
                if e is None: other_refs[n['id']].append(f'{f}:{L+1}')
                elif e['id'] == n['id'] or e['name'] == n['name']: pass
                else: edges_in[n['id']].add(e['id'])
            else: other_refs[n['id']].append(f'{f}:{L+1}')
    if cur: c.close(cur)
    log('closure: edges', round(time.time() - t0), 's')
    by_name = collections.defaultdict(list)
    for n in nodes:
        if n['kind'] in WANT: by_name[n['name']].append(n['id'])
    canon = {i: ids[0] for ids in by_name.values() for i in ids}
    Cn = lambda i: canon.get(i, i)
    out_edges = collections.defaultdict(set)
    for tgt, srcs in edges_in.items():
        for s in srcs: out_edges[Cn(s)].add(Cn(tgt))
    children = collections.defaultdict(list)
    for n in nodes:
        if n['parent'] is not None: children[Cn(n['parent'])].append(n['id'])
    def implied(i):
        out = set(out_edges.get(i, ())); n = nodes[i]
        if n['kind'] in ('class', 'struct'):
            for ch in children.get(i, ()):
                cn = nodes[ch]; last = cn['name'].split('::')[-1]
                if cn['kind'] == 'ctor' or last.startswith(('~', 'operator')) or 'override' in src_lines(cn['file'])[cn['line']] \
                   or cn['kind'] in ('class', 'struct', 'enum', 'alias'):
                    out.add(Cn(ch))
        if n['parent'] is not None and nodes[n['parent']]['kind'] in ('class', 'struct', 'enum'): out.add(Cn(n['parent']))
        return out
    live = set(); stack = []
    for i in list(root_refs) + list(other_refs):
        if Cn(i) not in live: live.add(Cn(i)); stack.append(Cn(i))
    while stack:
        i = stack.pop()
        for j in implied(i):
            if j not in live: live.add(j); stack.append(j)
    rows = []
    for name, ids in by_name.items():
        i = ids[0]; decl = next((nodes[j] for j in ids if nodes[j]['file'].endswith('.h')), nodes[i])
        st = 'LIVE' if i in live else ('TEST-ONLY' if any(test_refs.get(j) for j in ids) else 'DEAD')
        rows.append({'name': name, 'kind': decl['kind'], 'file': decl['file'], 'line': decl['line'] + 1, 'status': st,
                     'in_header': decl['file'].endswith('.h'),
                     'referrers': sorted({nodes[s]['name'] for j in ids for s in edges_in.get(j, ())}),
                     'tests': sorted({t.split(':')[0] for j in ids for t in test_refs.get(j, ())}),
                     'test_refs': sum(len(test_refs.get(j, ())) for j in ids),
                     'roots': sorted({x.split(':')[0] for j in ids for x in root_refs.get(j, ())})})
    json.dump(rows, open(os.path.join(SCR, 'closure.json'), 'w'), ensure_ascii=False, indent=0)
    log('closure summary:', dict(collections.Counter((r['status'], r['in_header']) for r in rows)))

# ------------------------------------------------------------------ macros
if 'macros' in modes:
    HEADERS = ['zametti-core/doc/settings.h', 'zametti-core/note/app_state.h']
    names = {}
    for h in HEADERS:
        for i, line in enumerate(src_lines(h)):
            m = re.match(r'\s*ZM_SETTING\w*\(\s*[^,]+,\s*(\w+)\s*,\s*(\w+)\s*,', line)
            if m:
                for g in (m.group(1), 'set' + m.group(2), m.group(1) + 'Min', m.group(1) + 'Max'): names[g] = (h, i + 1, m.group(1))
    files = sorted(glob.glob('zametti-core/*/*.[ch]*') + glob.glob('app/*.[ch]*') + glob.glob('store/*.cpp') + glob.glob('tests/*.[ch]*'))
    ident = re.compile(r'[A-Za-z_]\w*'); byfile = collections.defaultdict(list)
    for f in files:
        if f in HEADERS: continue
        for li, line in enumerate(src_lines(f)):
            code = re.sub(r'"(\\.|[^"\\])*"', '""', re.sub(r'//.*', '', line))
            for m in ident.finditer(code):
                if m.group(0) in names: byfile[f].append((m.group(0), li, m.start()))
    conf = collections.defaultdict(lambda: {'app': set(), 'core': set(), 'tests': set(), 'unconfirmed': []})
    for f in sorted(byfile):
        c.open(f)
        for (n, l, col) in byfile[f]:
            if names[n][0] in c.definition(f, l, col):
                conf[n]['app' if f.startswith(('app/', 'store/')) else 'tests' if f.startswith('tests/') else 'core'].add(f)
            else: conf[n]['unconfirmed'].append(f'{f}:{l+1}')
        c.close(f)
    rows = []
    for n, (h, line, setting) in sorted(names.items(), key=lambda x: (x[1][0], x[1][1], x[0])):
        cc = conf.get(n, {'app': set(), 'core': set(), 'tests': set(), 'unconfirmed': []})
        st = 'LIVE' if cc['app'] or cc['core'] else ('TEST-ONLY' if cc['tests'] else 'DEAD')
        rows.append({'name': n, 'header': h, 'line': line, 'setting': setting, 'status': st, 'app': sorted(cc['app']),
                     'core': sorted(cc['core']), 'tests': sorted(cc['tests']), 'unconfirmed': cc['unconfirmed']})
    json.dump(rows, open(os.path.join(SCR, 'macros.json'), 'w'), ensure_ascii=False, indent=0)
    log('macros summary:', dict(collections.Counter(r['status'] for r in rows)))

# ------------------------------------------------------------------ locals
if 'locals' in modes:
    files = sorted(glob.glob('app/*.cpp') + glob.glob('app/*.h') + glob.glob('store/*.cpp') + glob.glob('tests/*.cpp') + glob.glob('tests/*.h')
                   + glob.glob('zametti-core/*/*.cpp'))
    out = []
    for f in files:
        syms = c.symbols(f); src = src_lines(f)
        def walk(syms, path):
            for s in syms:
                k = KINDS.get(s['kind']); name = '::'.join(path + (s['name'],))
                if k in ('func', 'method', 'ctor', 'class', 'struct'):
                    pos = s['selectionRange']['start']; line = src[pos['line']] if pos['line'] < len(src) else ''
                    others = []
                    for r in c.refs(f, pos['line'], pos['character']):
                        rf = c.rel(r['uri']); L = r['range']['start']['line']
                        if rf == f and L == pos['line']: continue
                        others.append(f'{rf}:{L+1}')
                    out.append({'file': f, 'name': name, 'kind': k, 'line': pos['line'] + 1,
                                'local': '(anonymous namespace)' in name or re.match(r'\s*static\s', line) is not None,
                                'override': 'override' in line, 'n_refs': len(others), 'refs': sorted(set(others))[:8]})
                walk(s.get('children', []), path + (s['name'],))
        walk(syms, ()); c.close(f)
        log('locals:', f)
    json.dump(out, open(os.path.join(SCR, 'locals.json'), 'w'), ensure_ascii=False, indent=0)
    log('locals done', len(out))
c.stop()
