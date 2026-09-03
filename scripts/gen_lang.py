#!/usr/bin/env python3
import argparse
import json
import os

HEADER_TEMPLATE = """// Auto-generated language config
#pragma once

#ifndef {lang_code_for_font}
    #define {lang_code_for_font}
#endif

namespace Lang {{
    constexpr const char* CODE = "{lang_code}";

    namespace Strings {{
{strings}
    }}
}}
"""

def generate_header(input_path, output_path):
    with open(input_path, 'r', encoding='utf-8') as f:
        data = json.load(f)

    if 'language' not in data or 'strings' not in data:
        raise ValueError("Invalid JSON structure")

    lang_code = data['language']['type']

    strings = []
    for key, value in data['strings'].items():
        value = value.replace('"', '\\"')
        strings.append(f'        constexpr const char* {key.upper()} = "{value}";')

    content = HEADER_TEMPLATE.format(
        lang_code=lang_code,
        lang_code_for_font=lang_code.replace('-', '_').lower(),
        strings="\n".join(sorted(strings)),
    )

    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, 'w', encoding='utf-8') as f:
        f.write(content)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    generate_header(args.input, args.output)
