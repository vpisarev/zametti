# Клиент clangd по stdio для инвентаризации кода (см. README.md рядом).
#
# Индекс — СВОЙ, а не тот, которым пользуется плагин: база компиляции из одних
# наших файлов лежит в <работа>/cdb, и clangd кладёт фоновый индекс рядом с ней
# (<работа>/cdb/.cache/clangd/index). Причина в ловушке 23.08.2026: индекс
# плагина оказался ДЫРЯВЫМ — findReferences молчал про живой набор, потому что
# шард файла устарел и не переиндексировался. «Ноль ссылок» — доказательство
# только при проверенной полноте индекса.
#
# Перед вопросами ждём конца фоновой индексации ($/progress → end); стартует она
# лишь после первого didOpen, поэтому один файл открывается сразу.
import subprocess, json, os, sys, time, threading, queue
ROOT = os.getcwd()
# Каталог работы: туда ложатся база компиляции (cdb/), её индекс и отчёты.
# Задаётся ZAMETTI_INVENTORY_DIR; по умолчанию — рядом со скриптами.
SCR = os.environ.get('ZAMETTI_INVENTORY_DIR', os.path.dirname(os.path.abspath(__file__)))
CDB = os.path.join(SCR, 'cdb')

class Clangd:
    def __init__(self, jobs=4):
        self.p = subprocess.Popen(['clangd', '--background-index', f'--compile-commands-dir={CDB}',
                                   '--log=error', f'-j={jobs}', '--pch-storage=memory'],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.q = queue.Queue(); self.nid = 0; self.pending = {}
        self.progress = {'seen': False, 'done': False, 'pct': None}
        threading.Thread(target=self._reader, daemon=True).start()
        self.call('initialize', {'processId': os.getpid(), 'rootUri': self.uri(ROOT),
                                 'capabilities': {'window': {'workDoneProgress': True},
                                                  'textDocument': {'documentSymbol': {'hierarchicalDocumentSymbolSupport': True}}}})
        self.send('initialized', {}, notify=True)
        self.opened = set()
        # фоновая индексация стартует только после первого didOpen — будим её сразу
        self.open('zametti-core/doc/block_kind.h')
    def _reader(self):
        f = self.p.stdout
        while True:
            hdr = {}; line = f.readline()
            if not line: self.q.put(None); return
            while line.strip():
                k, v = line.decode().split(':', 1); hdr[k.strip()] = v.strip(); line = f.readline()
            self.q.put(json.loads(f.read(int(hdr['Content-Length']))))
    def raw(self, msg):
        b = json.dumps(msg).encode(); self.p.stdin.write(f'Content-Length: {len(b)}\r\n\r\n'.encode() + b); self.p.stdin.flush()
    def send(self, method, params, notify=False):
        msg = {'jsonrpc': '2.0', 'method': method, 'params': params}
        if not notify: self.nid += 1; msg['id'] = self.nid
        self.raw(msg); return None if notify else self.nid
    def pump(self, timeout=60):
        try: m = self.q.get(timeout=timeout)
        except queue.Empty: return False
        if m is None: raise SystemExit('clangd died')
        if 'id' in m and 'method' in m: self.raw({'jsonrpc': '2.0', 'id': m['id'], 'result': None})
        elif 'id' in m: self.pending[m['id']] = m
        elif m.get('method') == '$/progress':
            v = m['params'].get('value', {}); self.progress['seen'] = True
            if 'percentage' in v: self.progress['pct'] = v['percentage']
            if v.get('kind') == 'end': self.progress['done'] = True
        return True
    def call(self, method, params, timeout=600):
        i = self.send(method, params); t0 = time.time()
        while i not in self.pending:
            if not self.pump(5) and time.time() - t0 > timeout: raise SystemExit(f'timeout {method}')
        r = self.pending.pop(i)
        return ('ERR', r['error']) if 'error' in r else r.get('result')
    @staticmethod
    def uri(path): return 'file://' + os.path.abspath(path)
    @staticmethod
    def rel(u):
        f = u.replace('file://', '')
        return f[len(ROOT) + 1:] if f.startswith(ROOT + '/') else f
    def wait_index(self, max_s=3600):
        """Ждём конца фоновой индексации. Если прогресс так и не появился за 60 с — шарды
        уже на месте (вторая сессия) и индексировать нечего."""
        t0 = time.time(); last = None
        while time.time() - t0 < max_s:
            self.pump(5)
            if self.progress['pct'] != last:
                last = self.progress['pct']; print(f'  index {last}% ({round(time.time()-t0)} s)', file=sys.stderr, flush=True)
            if self.progress['done']: print('  index done', file=sys.stderr, flush=True); return True
            if not self.progress['seen'] and time.time() - t0 > 60: print('  no indexing needed', file=sys.stderr, flush=True); return True
        print('  WARNING: index wait timed out', file=sys.stderr); return False
    def open(self, f):
        if f in self.opened: return
        self.send('textDocument/didOpen', {'textDocument': {'uri': self.uri(f), 'languageId': 'cpp', 'version': 1,
                                                            'text': open(f, encoding='utf-8', errors='replace').read()}}, notify=True)
        self.opened.add(f)
    def close(self, f):
        if f in self.opened: self.send('textDocument/didClose', {'textDocument': {'uri': self.uri(f)}}, notify=True); self.opened.discard(f)
    def refs(self, f, line0, col0):
        r = self.call('textDocument/references', {'textDocument': {'uri': self.uri(f)}, 'position': {'line': line0, 'character': col0},
                                                  'context': {'includeDeclaration': True}})
        return r if isinstance(r, list) else []
    def definition(self, f, line0, col0):
        r = self.call('textDocument/definition', {'textDocument': {'uri': self.uri(f)}, 'position': {'line': line0, 'character': col0}})
        if not isinstance(r, list): return []
        return [self.rel(x.get('uri') or x.get('targetUri')) for x in r]
    def symbols(self, f):
        self.open(f)
        r = self.call('textDocument/documentSymbol', {'textDocument': {'uri': self.uri(f)}})
        return r if isinstance(r, list) else []
    def stop(self): self.p.terminate()
