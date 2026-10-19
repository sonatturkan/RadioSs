#!/usr/bin/env python3
"""Compile compatible declarations from the original design CSS into C.

Development/build tool only. CSS grid/flex geometry is implemented by GtkGrid/
GtkBox in the C views. Original stylesheets and load order remain authoritative.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DESIGN = ROOT / "design/reference/tasarrım"
PROPERTIES = {
    "color", "background", "background-color", "background-image", "border", "border-color", "border-width",
    "border-style", "border-top", "border-bottom", "border-left", "border-right",
    "border-top-color", "border-bottom-color", "border-left-color", "border-right-color",
    "border-radius", "padding", "padding-top", "padding-bottom", "padding-left",
    "padding-right", "margin", "margin-top", "margin-bottom", "margin-left",
    "margin-right", "font-size", "font-weight", "font-style", "opacity",
    "min-width", "min-height",
}

def gtk_colors(value: str) -> str:
    def replace(match: re.Match) -> str:
        digits = match[1]
        if len(digits) == 4:
            digits = ''.join(char * 2 for char in digits)
        if len(digits) != 8:
            return match[0]
        red, green, blue, alpha = (int(digits[i:i+2], 16) for i in (0, 2, 4, 6))
        return f'rgba({red},{green},{blue},{alpha/255:.6f})'
    return re.sub(r'#([0-9a-fA-F]{8}|[0-9a-fA-F]{4})(?![0-9a-fA-F])', replace, value)

def remove_at_blocks(css: str) -> str:
    """Do not apply narrow-screen rules to every native window."""
    while match := re.search(r"@[\w-]+[^;{}]*\{", css):
        depth, end = 1, match.end()
        while end < len(css) and depth:
            depth += (css[end] == "{") - (css[end] == "}")
            end += 1
        css = css[:match.start()] + css[end:]
    return css

def selector_to_gtk(selector: str) -> str | None:
    selector = selector.strip()
    if not selector or any(x in selector for x in ("@", "::", ":has", ":is", ":where", ":empty", ":invalid", ":user-invalid", ":placeholder-shown")):
        return None
    selector = re.sub(r'\[aria-selected=["\']true["\']\]', ':checked', selector)
    selector = re.sub(r'\[aria-pressed=["\']true["\']\]', ':checked', selector)
    selector = selector.replace(":focus-visible", ":focus")
    selector = selector.replace(":nth-of-type", ":nth-child")
    selector = selector.replace(":first-of-type", ":first-child")
    selector = selector.replace(":last-of-type", ":last-child")
    selector = selector.replace(":only-of-type", ":only-child")
    if '[' in selector or '%' in selector:
        return None
    # Match element tokens, never words inside a class or GTK widget ID. A word
    # boundary alone would corrupt .header-clock-time or .ld-players-body.
    nodes = {
        'body': '.radios', 'input': 'entry', 'select': 'combobox',
        **{f'h{level}': 'label.heading' for level in range(1, 7)},
        'p': 'label.paragraph', 'span': 'label.span', 'small': 'label.small',
        'output': 'label.output', 'time': 'label.time',
    }
    selector = re.sub(
        r'(^|[\s>+~,(])(' + '|'.join(nodes) + r')(?=$|[\s>+~,.#:\[)])',
        lambda match: match[1] + nodes[match[2]], selector)
    if not selector.startswith('.radios'):
        selector = '.radios ' + selector
    # A browser dialog descends from body; a GtkDialog is a separate root.
    # Keep its source ID/class on that root instead of requiring an ancestor
    # that cannot exist across the native transient-window boundary.
    dialog_root = r'(?:\.(?:ops-dialog|playlist-dialog|programme-dialog|pw-dialog|technical-dialog|advert-dialog|settings-dialog|transition-editor-dialog|qwf-dialog|dj-sampler-dialog)|#(?:queue-waveform-dialog|transition-editor))(?=$|[\s.#:>+~])'
    selector = re.sub(r'^\.radios (?=' + dialog_root + ')', '.radios', selector)
    return selector

def split_selectors(value: str) -> list[str]:
    result, start, depth = [], 0, 0
    for index, char in enumerate(value):
        depth += (char in '([') - (char in ')]')
        if char == ',' and depth == 0:
            result.append(value[start:index])
            start = index + 1
    result.append(value[start:])
    return result

def expand_selector(value: str) -> list[str]:
    match = re.search(r':(?:is|where)\(([^()]+)\)', value)
    if not match:
        return [value]
    return [expanded for option in split_selectors(match[1])
            for expanded in expand_selector(value[:match.start()] + option.strip() + value[match.end():])]

def gtk_linear_gradient(value: str) -> str:
    # GTK 3 supports one position per stop. Expand CSS multi-position stops
    # into two stops with the same source color, preserving the visual result.
    stops = []
    position = r'-?(?:\d+(?:\.\d*)?|\.\d+)(?:%|px|pt)?'
    for item in split_selectors(value[len('linear-gradient('):-1]):
        item = item.strip()
        match = re.fullmatch(r'(.+?)\s+(' + position + r')\s+(' + position + r')', item)
        if match:
            for location in (match[2], match[3]):
                stops.append(f'{match[1]} {"0%" if location == "0" else location}')
        else:
            stops.append(item)
    return 'linear-gradient(' + ', '.join(stops) + ')'

def compile_css() -> tuple[str, dict]:
    html = (DESIGN / "index.html").read_text()
    sources = [("index.html#style", "\n".join(re.findall(r"<style[^>]*>(.*?)</style>", html, re.S)))]
    for href in re.findall(r'<link[^>]+href="([^"]+\.css(?:\?[^"]*)?)"', html):
        path = DESIGN / href.split('?')[0]
        sources.append((path.name, path.read_text()))
    # A custom property can be flattened safely only when every source defines
    # the same literal value. Scope-dependent theme/knob variables remain out.
    variable_values: dict[str, set[str]] = {}
    for _, source in sources:
        for name, value in re.findall(r'(--[\w-]+)\s*:\s*([^;}]+)', source):
            variable_values.setdefault(name, set()).add(value.strip())
    variables = {name: next(iter(values)) for name, values in variable_values.items()
                 if len(values) == 1}
    def resolve_variables(value: str) -> str:
        for _ in range(len(variables) + 1):
            resolved = re.sub(r'var\(\s*(--[\w-]+)\s*\)',
                              lambda match: variables.get(match[1], match[0]), value)
            if resolved == value:
                break
            value = resolved
        return value

    output = ["/* Generated from original design CSS; see tools/css_to_gtk.py. */"]
    source_records, rules = [], 0
    for name, source in sources:
        source_records.append({"source": name, "sha256": hashlib.sha256(source.encode()).hexdigest()})
        css = remove_at_blocks(re.sub(r"/\*.*?\*/", "", source, flags=re.S))
        output.append(f"/* Source: {name} */")
        for match in re.finditer(r"([^{}]+)\{([^{}]*)\}", css):
            declarations = []
            for declaration in match[2].split(';'):
                key, separator, value = declaration.partition(':')
                key, value = key.strip(), value.strip().replace('!important', '').strip()
                value = gtk_colors(resolve_variables(value))
                if not separator or key not in PROPERTIES or not value:
                    continue
                if re.search(r"var\(|calc\(|clamp\(|min\(|max\(|inherit|unset|initial|auto|[\d.](?:vw|vh|em|rem)", value):
                    continue
                if key in ('background', 'background-image'):
                    if value.startswith('linear-gradient(') and value.endswith(')'):
                        key = 'background-image'
                        value = gtk_linear_gradient(value)
                    elif key == 'background' and (
                            re.fullmatch(r'#[\da-fA-F]{3,8}|rgba?\([^()]+\)|transparent', value)):
                        key = 'background-color'
                    elif value == 'none':
                        key = 'background-image'
                    else:
                        continue
                elif re.search(r'[\d.]%', value):
                    continue
                if key in ('font-size', 'min-width', 'min-height') and not re.fullmatch(r"[\d.]+(?:px|pt)", value):
                    continue
                if key == 'font-weight' and value not in ('normal', 'bold', 'bolder', 'lighter'):
                    try:
                        value = str(min(900, max(100, round(float(value) / 100) * 100)))
                    except ValueError:
                        continue
                declarations.append(f"{key}: {value};")
            selectors = [selector_to_gtk(s) for group in split_selectors(match[1])
                         for s in expand_selector(group)]
            selectors = [s for s in selectors if s is not None]
            if selectors and declarations:
                output.append(', '.join(selectors) + ' { ' + ' '.join(declarations) + ' }')
                rules += 1
    return '\n'.join(output) + '\n', {"sources": source_records, "native_rules": rules}

def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    css, manifest = compile_css()
    (args.output_dir / 'reference.css').write_text(css)
    lines = ['#include <glib.h>', 'const gchar *const radios_reference_css_parts[] = {']
    lines += [json.dumps(css[index:index+2000], ensure_ascii=False) + ','
              for index in range(0, len(css), 2000)]
    lines += ['NULL };']
    (args.output_dir / 'reference-css.c').write_text('\n'.join(lines) + '\n')
    (args.output_dir / 'css-manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')
    print(f"Design CSS: {len(manifest['sources'])} sources, {manifest['native_rules']} native rules")

if __name__ == '__main__':
    main()
