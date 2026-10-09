"""Checks that plugin/kenshi.cpp's kFunctions table lists the functions in the order of the Fn enum
in plugin/kenshi.h (a swapped pair calls the wrong function; the prologue check cannot see it).

    python tools/check_functions.py      exit code 0: every entry matches its enum comment
"""
import os
import re
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
h = open(os.path.join(root, 'plugin', 'kenshi.h'), encoding='utf-8').read()
c = open(os.path.join(root, 'plugin', 'kenshi.cpp'), encoding='utf-8').read()
body = h[h.find('enum Fn : int {') + len('enum Fn : int {'):h.find('    FnCount')]
enum = []
for line in body.split('\n'):
    code, _, comment = line.partition('//')
    enum += [(tok, comment.strip()) for tok in re.findall(r'(Fn\w+)', code)]
table = c[c.find('const FunctionSig kFunctions[FnCount] = {'):]
table = re.findall(r'\{"([^"]+)", 0x([0-9A-Fa-f]+)', table[:table.find('\n};')])
bad = 0
if len(enum) != len(table):
    print(f'{len(enum)} enum entries, {len(table)} table entries')
    bad += 1
for i, ((fn, comment), (name, rva)) in enumerate(zip(enum, table)):
    key = name.split('::')[-1].split('(')[0].split(' ')[0].lower()
    if key not in comment.lower() and key not in fn.lower():
        bad += 1
        print(f'#{i} {fn} ({comment[:60]}) <- table entry "{name}" 0x{rva}')
print('ok' if not bad else f'{bad} mismatch(es)')
sys.exit(1 if bad else 0)
