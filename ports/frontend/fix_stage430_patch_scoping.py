#!/usr/bin/env python3
from pathlib import Path

p = Path('/tmp/patch_stage430_full_menu_hotkeys.py')
s = p.read_text(encoding='utf-8')

needle = '''def replace_between(text, start, end, repl):
    a = text.find(start)
    b = text.find(end, a + len(start))
    if a < 0 or b < 0:
        raise SystemExit(f'anchor missing: {start} / {end}')
    return text[:a] + repl.rstrip() + '\\n\\n' + text[b:]
'''
helper = needle + '''

def replace_function(text, signature, repl):
    a = text.find(signature)
    if a < 0:
        raise SystemExit(f'function anchor missing: {signature}')
    brace = text.find('{', a + len(signature))
    if brace < 0:
        raise SystemExit(f'function opening brace missing: {signature}')
    depth = 0
    i = brace
    in_string = False
    in_char = False
    escape = False
    line_comment = False
    block_comment = False
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ''
        if line_comment:
            if c == '\\n': line_comment = False
        elif block_comment:
            if c == '*' and n == '/': block_comment = False; i += 1
        elif in_string:
            if escape: escape = False
            elif c == '\\\\': escape = True
            elif c == '"': in_string = False
        elif in_char:
            if escape: escape = False
            elif c == '\\\\': escape = True
            elif c == "'": in_char = False
        else:
            if c == '/' and n == '/': line_comment = True; i += 1
            elif c == '/' and n == '*': block_comment = True; i += 1
            elif c == '"': in_string = True
            elif c == "'": in_char = True
            elif c == '{': depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0:
                    return text[:a] + repl.rstrip() + '\\n\\n' + text[i + 1:]
        i += 1
    raise SystemExit(f'function closing brace missing: {signature}')
'''

if 'def replace_function(' not in s:
    if needle not in s:
        raise SystemExit('replace_between helper block not found')
    s = s.replace(needle, helper, 1)

s = s.replace(
    "src = replace_between(src, 'static void stage413_menu_draw(', 'static void stage42_draw_system_row(', menu_block)",
    "src = replace_function(src, 'static void stage413_menu_draw(', menu_block)")
s = s.replace(
    "src = replace_between(src, 'static void stage413_quick_menu(', 'static int stage42_layout_test()', quick)",
    "src = replace_function(src, 'static void stage413_quick_menu(', quick)")

p.write_text(s, encoding='utf-8')
print('STAGE430_PATCH_SCOPE_FIX_OK')
