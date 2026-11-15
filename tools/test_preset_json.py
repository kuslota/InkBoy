#!/usr/bin/env python3
"""Encode and decode inkboy-preset-v1 against an independent Python rendering and every rejection path."""
import copy
import json

from host_build import CJSON, run

SHADES = 'KDLW'
ROUTES = ['L>K>L', 'D>K>D', 'D>W>D', 'L>W>L', 'D>L>D', 'L>D>L']
SCAN = ['auto', 'one']
STRENGTH = ['stock', '-13V', '-11V', '-9V']
HISTORY = ['off', 'returns', 'reversals', 'both']
WEAK = ['normal', '11V', '8.8V']

DITHERED = {'black_pulses': 8, 'white_pulses': 8}
DEFAULT_GRAY_HOLDS = 50791
DEFAULT_GRAY = {
    'doses': {'K': [0, 2, 4, 7], 'D': [4, 0, 1, 6], 'L': [4, 1, 0, 5], 'W': [7, 3, 2, 0]},
    'retarget_percent': 100, 'scan_policy': 'auto', 'strength': 'stock', 'pair_hold': True,
    'holds': {s: [bool(DEFAULT_GRAY_HOLDS >> (i * 4 + j) & 1) for j in range(4)] for i, s in enumerate(SHADES)},
    'history': 'both', 'weak_white_to_light': 'normal',
    'routes': {r: {'return': -1 if r == 'L>K>L' else 0, 'reverse_percent': 150 if r == 'L>K>L' else None} for r in ROUTES},
}
ALT = {
    'doses': {'K': [0, 1, 12, 3], 'D': [5, 0, 6, 7], 'L': [8, 9, 0, 10], 'W': [11, 2, 4, 0]},
    'retarget_percent': 150, 'scan_policy': 'one', 'strength': '-11V', 'pair_hold': False,
    'holds': {'K': [True, False, True, True], 'D': [False, True, True, False],
              'L': [True, False, True, False], 'W': [False, True, False, True]},
    'history': 'reversals', 'weak_white_to_light': '8.8V',
    'routes': {'L>K>L': {'return': -3, 'reverse_percent': 50}, 'D>K>D': {'return': 3, 'reverse_percent': None},
               'D>W>D': {'return': 0, 'reverse_percent': 75}, 'L>W>L': {'return': 2, 'reverse_percent': 200},
               'D>L>D': {'return': -1, 'reverse_percent': 125}, 'L>D>L': {'return': 1, 'reverse_percent': 175}},
}


def render(mode, name, settings):
    head = f'{{\n  "format": "inkboy-preset-v1",\n  "mode": "{mode}",\n  "name": "{name}",\n  "settings": '
    if mode == 'dithered':
        return head + f'{{ "black_pulses": {settings["black_pulses"]}, "white_pulses": {settings["white_pulses"]} }}\n}}\n'
    t = lambda b: 'true' if b else 'false'
    doses = ',\n'.join(f'      "{s}": {json.dumps(settings["doses"][s])}' for s in SHADES)
    holds = ',\n'.join(f'      "{s}": [{", ".join(t(b) for b in settings["holds"][s])}]' for s in SHADES)
    routes = ',\n'.join(
        f'      "{r}": {{ "return": {settings["routes"][r]["return"]}, "reverse_percent": '
        f'{"null" if settings["routes"][r]["reverse_percent"] is None else settings["routes"][r]["reverse_percent"]} }}'
        for r in ROUTES)
    return (head + '{\n    "doses": {\n' + doses + '\n    },\n'
            f'    "retarget_percent": {settings["retarget_percent"]},\n'
            f'    "scan_policy": "{settings["scan_policy"]}",\n    "strength": "{settings["strength"]}",\n'
            f'    "pair_hold": {t(settings["pair_hold"])},\n    "holds": {{\n{holds}\n    }},\n'
            f'    "history": "{settings["history"]}",\n    "weak_white_to_light": "{settings["weak_white_to_light"]}",\n'
            f'    "routes": {{\n{routes}\n    }}\n  }}\n}}\n')


def profile_initializer(s):
    mask = sum(1 << (i * 4 + j) for i, sh in enumerate(SHADES) for j in range(4) if s['holds'][sh][j])
    doses = ','.join('{' + ','.join(map(str, s['doses'][sh])) + '}' for sh in SHADES)
    routes = [s['routes'][r] for r in ROUTES]
    returns = ','.join(str(r['return']) for r in routes)
    reverses = ','.join(str(r['reverse_percent'] or 0) for r in routes)
    return (f'{{.version=3,.tuning={{1,{{{doses}}},{(s["retarget_percent"] - 50) // 25},{SCAN.index(s["scan_policy"])}}},'
            f'.strength={STRENGTH.index(s["strength"])},.immediate={int(s["pair_hold"])},.holds_lo={mask & 255},.holds_hi={mask >> 8},'
            f'.adaptive={{{HISTORY.index(s["history"])},{WEAK.index(s["weak_white_to_light"])},{{{returns}}},{{{reverses}}}}}}}')


def c_string(data):
    out = []
    for b in data:
        out.append(chr(b) if 32 <= b < 127 and chr(b) not in '"\\?' else '\\%03o' % b)
    return '"' + ''.join(out) + '"'


def document(mode='grayscale', settings=None, **top):
    base = {'format': 'inkboy-preset-v1', 'mode': mode, 'name': 'Test', 'settings': copy.deepcopy(settings or (DEFAULT_GRAY if mode == 'grayscale' else DITHERED))}
    base.update(top)
    return base


cases = []


def case(label, mode, text, expected):
    data = text if isinstance(text, bytes) else text.encode()
    cases.append((label, 'DISPLAY_' + mode.upper(), data, 'PRESET_JSON_' + expected))


def mutate(label, mode, edit, expected, settings=None):
    doc = document(mode, settings)
    edit(doc)
    case(label, mode, json.dumps(doc, indent=2), expected)


def leaf_paths(node, path=()):
    if isinstance(node, dict):
        for key in node:
            yield from leaf_paths(node[key], path + (key,))
    elif isinstance(node, list):
        for i in range(len(node)):
            yield from leaf_paths(node[i], path + (i,))
    else:
        yield path


def walk(doc, path):
    for step in path[:-1]:
        doc = doc[step]
    return doc


def set_path(doc, path, value):
    walk(doc, path)[path[-1]] = value


def delete_path(doc, path):
    del walk(doc, path)[path[-1]]


for mode, settings in (('dithered', DITHERED), ('grayscale', DEFAULT_GRAY)):
    case(f'{mode} accepted', mode, json.dumps(document(mode), indent=2), 'OK')
    case(f'{mode} unknown keys ignored', mode, json.dumps(document(mode, comment='x', extra={'a': [1, 2]}), indent=2), 'OK')
    mutate(f'{mode} unknown settings key ignored', mode, lambda d: d['settings'].update(future=[True]), 'OK')
    mutate(f'{mode} wrong format', mode, lambda d: d.update(format='inkboy-preset-v2'), 'WRONG_FORMAT')
    mutate(f'{mode} numeric format', mode, lambda d: d.update(format=1), 'WRONG_FORMAT')
    mutate(f'{mode} missing format', mode, lambda d: d.pop('format'), 'MISSING_KEY')
    mutate(f'{mode} key case matters', mode, lambda d: d.update(Format=d.pop('format')), 'MISSING_KEY')
    other = 'grayscale' if mode == 'dithered' else 'dithered'
    mutate(f'{mode} file of the other mode', mode, lambda d: d.update(mode=other), 'WRONG_MODE')
    mutate(f'{mode} unknown mode', mode, lambda d: d.update(mode='sepia'), 'BAD_VALUE')
    mutate(f'{mode} missing mode', mode, lambda d: d.pop('mode'), 'MISSING_KEY')
    mutate(f'{mode} missing name', mode, lambda d: d.pop('name'), 'MISSING_KEY')
    for bad in ('', 'x' * 21, 'say "hi"', 'back\\slash', 'ctrl\x01', ' lead', 'trail ', 5, None):
        mutate(f'{mode} bad name {bad!r}', mode, lambda d, bad=bad: d.update(name=bad), 'BAD_VALUE')
    mutate(f'{mode} longest name', mode, lambda d: d.update(name='x' * 20), 'OK')
    mutate(f'{mode} missing settings', mode, lambda d: d.pop('settings'), 'MISSING_KEY')
    for bad in ([], 'text', 3, None):
        mutate(f'{mode} settings not an object {bad!r}', mode, lambda d, bad=bad: d.update(settings=bad), 'BAD_VALUE')
    for path in leaf_paths(settings):
        is_item = isinstance(path[-1], int)
        mutate(f'{mode} without {path}', mode, lambda d, path=path: delete_path(d['settings'], path),
               'BAD_VALUE' if is_item else 'MISSING_KEY')
    for key in ('doses', 'holds', 'routes') if mode == 'grayscale' else ():
        mutate(f'grayscale {key} not an object', mode, lambda d, key=key: d['settings'].update({key: [1]}), 'BAD_VALUE')

for key in ('black_pulses', 'white_pulses'):
    for bad in (0, 9, -1, 2.5, 256, '3', None, True, [3]):
        mutate(f'dithered {key}={bad!r}', 'dithered', lambda d, key=key, bad=bad: d['settings'].update({key: bad}), 'BAD_VALUE')
    for good in (1, 8, 4):
        mutate(f'dithered {key}={good}', 'dithered', lambda d, key=key, good=good: d['settings'].update({key: good}), 'OK')
case('dithered integers written as 8.0', 'dithered', json.dumps(document('dithered')).replace('8', '8.0'), 'OK')

gray_bad = [
    (('retarget_percent',), (0, 40, 49, 60, 151, 175, 200, 100.5, '100', None, True)),
    (('scan_policy',), ('two', 'AUTO', 1, None)),
    (('strength',), ('-15V', 'STOCK', 'weaker', 0, None)),
    (('pair_hold',), (1, 0, 'true', None)),
    (('history',), ('everything', 'Both', 2, None)),
    (('weak_white_to_light',), ('9V', '11v', 1, None)),
]
for path, values in gray_bad:
    for bad in values:
        mutate(f'grayscale {path[0]}={bad!r}', 'grayscale', lambda d, path=path, bad=bad: set_path(d['settings'], path, bad), 'BAD_VALUE')
for value in (50, 75, 100, 125, 150):
    mutate(f'grayscale retarget {value}', 'grayscale', lambda d, value=value: set_path(d['settings'], ('retarget_percent',), value), 'OK')
for shade_index, shade in enumerate(SHADES):
    for to in range(4):
        valid = [0] if shade_index == to else [1, 12]
        invalid = [1, 12, -1, 2.5, '3', None, True] if shade_index == to else [0, 13, -1, 100, 2.5, '3', None, 256, True]
        for bad in invalid:
            mutate(f'grayscale dose {shade}->{SHADES[to]}={bad!r}', 'grayscale',
                   lambda d, shade=shade, to=to, bad=bad: set_path(d['settings'], ('doses', shade, to), bad), 'BAD_VALUE')
        for good in valid:
            mutate(f'grayscale dose {shade}->{SHADES[to]}={good}', 'grayscale',
                   lambda d, shade=shade, to=to, good=good: set_path(d['settings'], ('doses', shade, to), good), 'OK')
    mutate(f'grayscale {shade} dose row too long', 'grayscale', lambda d, shade=shade: d['settings']['doses'][shade].append(1), 'BAD_VALUE')
    mutate(f'grayscale {shade} dose row not an array', 'grayscale', lambda d, shade=shade: d['settings']['doses'].update({shade: 3}), 'BAD_VALUE')
    mutate(f'grayscale {shade} hold row too long', 'grayscale', lambda d, shade=shade: d['settings']['holds'][shade].append(True), 'BAD_VALUE')
    for to in range(4):
        for bad in (1, 0, 'true', None):
            mutate(f'grayscale hold {shade}->{SHADES[to]}={bad!r}', 'grayscale',
                   lambda d, shade=shade, to=to, bad=bad: set_path(d['settings'], ('holds', shade, to), bad), 'BAD_VALUE')
        mutate(f'grayscale hold {shade}->{SHADES[to]}=false', 'grayscale',
               lambda d, shade=shade, to=to: set_path(d['settings'], ('holds', shade, to), False),
               'BAD_VALUE' if shade_index == to else 'OK')
        mutate(f'grayscale hold {shade}->{SHADES[to]}=true', 'grayscale',
               lambda d, shade=shade, to=to: set_path(d['settings'], ('holds', shade, to), True), 'OK')
for route in ROUTES:
    for bad in (-4, 4, 1.5, '1', None, 200, True):
        mutate(f'grayscale {route} return={bad!r}', 'grayscale',
               lambda d, route=route, bad=bad: set_path(d['settings'], ('routes', route, 'return'), bad), 'BAD_VALUE')
    for good in (-3, 0, 3):
        mutate(f'grayscale {route} return={good}', 'grayscale',
               lambda d, route=route, good=good: set_path(d['settings'], ('routes', route, 'return'), good), 'OK')
    for bad in (0, 1, 49, 51, 60, 201, 225, 250, 256, -50, 100.5, '150', True, [150]):
        mutate(f'grayscale {route} reverse_percent={bad!r}', 'grayscale',
               lambda d, route=route, bad=bad: set_path(d['settings'], ('routes', route, 'reverse_percent'), bad), 'BAD_VALUE')
    for good in (50, 75, 100, 125, 150, 175, 200, None):
        mutate(f'grayscale {route} reverse_percent={good}', 'grayscale',
               lambda d, route=route, good=good: set_path(d['settings'], ('routes', route, 'reverse_percent'), good), 'OK')
    mutate(f'grayscale {route} entry not an object', 'grayscale',
           lambda d, route=route: d['settings']['routes'].update({route: 3}), 'BAD_VALUE')
for index, name in enumerate(HISTORY):
    mutate(f'grayscale history {name}', 'grayscale', lambda d, name=name: set_path(d['settings'], ('history',), name), 'OK')
for name in STRENGTH:
    mutate(f'grayscale strength {name}', 'grayscale', lambda d, name=name: set_path(d['settings'], ('strength',), name), 'OK')
for name in WEAK:
    mutate(f'grayscale weak {name}', 'grayscale', lambda d, name=name: set_path(d['settings'], ('weak_white_to_light',), name), 'OK')
for name in SCAN:
    mutate(f'grayscale scan {name}', 'grayscale', lambda d, name=name: set_path(d['settings'], ('scan_policy',), name), 'OK')

good = render('dithered', 'Test', DITHERED)
for label, text in (('empty', ''), ('open brace', '{'), ('plain text', 'not json'), ('array root', '[]'), ('null root', 'null'),
                    ('number root', '7'), ('trailing text', good + 'x'), ('trailing object', good + '{}'),
                    ('truncated', good[:-4]), ('single quotes', good.replace('"', "'")),
                    ('trailing comma', good.replace('8 }', '8, }')), ('comment', '// hi\n' + good),
                    ('deep nesting', '[' * 100), ('deep unknown key', json.dumps(document('dithered', deep=[[[[[[[[[1]]]]]]]]]))),
                    ('bracket in string', json.dumps(document('dithered', note='[[[[[[[[[[[[')))):
    expected = 'OK' if label == 'bracket in string' else 'MALFORMED'
    case(f'malformed: {label}', 'dithered', text, expected)
case('malformed: shallow unknown key', 'dithered', json.dumps(document('dithered', deep=[[[1]]])), 'OK')
case('malformed: embedded NUL', 'dithered', good[:10].encode() + b'\0' + good[10:].encode(), 'MALFORMED')
case('malformed: binary', 'dithered', bytes(range(1, 200)), 'MALFORMED')

pad = lambda text, size: text[:-1] + ' ' * (size - len(text)) + '}'
base_text = json.dumps(document('dithered'))
case('exactly 8192 bytes', 'dithered', pad(base_text, 8192), 'OK')
case('8193 bytes', 'dithered', pad(base_text, 8193), 'TOO_LARGE')
case('large and invalid json', 'dithered', 'x' * 9000, 'TOO_LARGE')

case_rows = ',\n'.join(
    f'    {{{c_string(label.encode())}, {c_string(data)}, {len(data)}, {mode}, {status}}}' for label, mode, data, status in cases)
gray_text = render('grayscale', 'Default', DEFAULT_GRAY)
alt_text = render('grayscale', 'Alt Two', ALT)
dithered_text = render('dithered', 'Default', DITHERED)
assert json.loads(dithered_text)['settings'] == DITHERED and json.loads(gray_text)['settings'] == DEFAULT_GRAY
assert json.loads(alt_text)['settings'] == ALT

CODE = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "preset_json.h"

typedef struct { const char *label; const char *text; size_t length; display_mode_t mode; preset_json_status_t expected; } decode_case_t;
static const decode_case_t CASES[] = {
''' + case_rows + r'''
};

static void decode_cases(void)
{
    for (unsigned i = 0; i < sizeof(CASES) / sizeof(CASES[0]); ++i) {
        const decode_case_t *c = &CASES[i];
        char *copy = malloc(c->length + 1);
        memcpy(copy, c->text, c->length);
        copy[c->length] = 0;
        display_preset_t sentinel, preset;
        memset(&sentinel, 0xa5, sizeof(sentinel));
        preset = sentinel;
        preset_json_status_t status = preset_json_decode(copy, c->length, c->mode, &preset);
        if (status != c->expected) { fprintf(stderr, "case '%s': got %s\n", c->label, preset_json_status_name(status)); assert(0); }
        if (status == PRESET_JSON_OK) assert(display_preset_valid(&preset) && preset.mode == c->mode);
        else assert(!memcmp(&preset, &sentinel, sizeof(preset))); /* A rejected file never leaves partial values behind. */
        free(copy);
    }
}

static display_preset_t gray_preset(const char *name, const gray_profile_t *profile)
{
    display_preset_t p = {.mode = DISPLAY_GRAYSCALE, .gray = *profile};
    snprintf(p.name, sizeof(p.name), "%s", name);
    assert(display_preset_valid(&p));
    return p;
}

static void check_text(const display_preset_t *preset, const char *expected)
{
    char json[PRESET_JSON_MAX];
    size_t length;
    assert(preset_json_encode(preset, json, sizeof(json), &length));
    assert(length == strlen(json));
    if (strcmp(json, expected)) { fprintf(stderr, "--- got\n%s--- expected\n%s", json, expected); assert(0); }
    display_preset_t decoded;
    assert(preset_json_decode(json, length, preset->mode, &decoded) == PRESET_JSON_OK);
    assert(display_preset_same_settings(&decoded, preset) && !strcmp(decoded.name, preset->name));
}

static void exact_text_and_values(void)
{
    check_text(display_preset_default(DISPLAY_DITHERED), ''' + c_string(dithered_text.encode()) + r''');
    check_text(display_preset_default(DISPLAY_GRAYSCALE), ''' + c_string(gray_text.encode()) + r''');
    static const gray_profile_t alt = ''' + profile_initializer(ALT) + r''';
    display_preset_t preset = gray_preset("Alt Two", &alt);
    check_text(&preset, ''' + c_string(alt_text.encode()) + r''');
    display_preset_t decoded;
    assert(preset_json_decode(''' + c_string(alt_text.encode()) + r''', ''' + str(len(alt_text)) + r''', DISPLAY_GRAYSCALE, &decoded) == PRESET_JSON_OK);
    assert(!memcmp(&decoded.gray, &alt, sizeof(alt)) && !decoded.black_pulses && !decoded.white_pulses);
    static const gray_profile_t default_gray = ''' + profile_initializer(DEFAULT_GRAY) + r''';
    assert(!memcmp(&display_preset_default(DISPLAY_GRAYSCALE)->gray, &default_gray, sizeof(default_gray)));
}

static void encoder_limits(void)
{
    char json[PRESET_JSON_MAX];
    size_t length;
    const display_preset_t *gray = display_preset_default(DISPLAY_GRAYSCALE);
    assert(preset_json_encode(gray, json, sizeof(json), &length) && length < PRESET_JSON_MAX && length < PRESET_FILE_MAX);
    for (size_t size = 1; size <= length; ++size) assert(!preset_json_encode(gray, json, size, NULL));
    assert(preset_json_encode(gray, json, length + 1, NULL));
    display_preset_t bad = *gray;
    bad.gray.strength = 4;
    assert(!preset_json_encode(&bad, json, sizeof(json), &length));
    bad = *display_preset_default(DISPLAY_DITHERED);
    bad.white_pulses = 9;
    assert(!preset_json_encode(&bad, json, sizeof(json), &length));
    assert(!preset_json_encode(NULL, json, sizeof(json), &length) && !preset_json_encode(gray, NULL, 10, &length));
    for (unsigned i = 0; i <= PRESET_JSON_BAD_VALUE; ++i) assert(strcmp(preset_json_status_name((preset_json_status_t)i), "unknown"));
    assert(!strcmp(preset_json_status_name((preset_json_status_t)99), "unknown"));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "decode")) decode_cases();
    else if (!strcmp(argv[1], "text")) exact_text_and_values();
    else if (!strcmp(argv[1], "limits")) encoder_limits();
    else assert(0);
    return 0;
}
'''

run(CODE, modules=['display_preset.c', 'gray_profile_store.c', 'preset_json.c'], vendor_sources=[CJSON],
    scenarios=[[name] for name in ('decode', 'text', 'limits')])
print(f'PASS: preset JSON text and values match an independent rendering; {len(cases)} accept/reject cases; encoder bounds')
