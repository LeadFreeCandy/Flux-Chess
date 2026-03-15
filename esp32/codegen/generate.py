#!/usr/bin/env python3
"""Reads firmware/api.h and generates frontend/src/generated/api.ts."""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
API_H = ROOT / "firmware" / "api.h"
OUT_TS = ROOT / "frontend" / "src" / "generated" / "api.ts"

# C++ type -> TypeScript type
TYPE_MAP = {
    "uint8_t": "number",
    "uint16_t": "number",
    "uint32_t": "number",
    "int8_t": "number",
    "int16_t": "number",
    "int32_t": "number",
    "int": "number",
    "float": "number",
    "double": "number",
    "bool": "boolean",
    "String": "string",
}


def parse_api_header(text: str):
    enums = []
    structs = []
    commands = []

    # Parse enum class with underlying type
    for m in re.finditer(
        r"enum\s+class\s+(\w+)\s*:\s*\w+\s*\{([^}]+)\}", text
    ):
        name = m.group(1)
        values = [v.strip() for v in m.group(2).split(",") if v.strip()]
        enums.append((name, values))

    # Also match enum class without underlying type
    for m in re.finditer(
        r"enum\s+class\s+(\w+)\s*\{([^}]+)\}", text
    ):
        name = m.group(1)
        if any(e[0] == name for e in enums):
            continue
        values = [v.strip() for v in m.group(2).split(",") if v.strip()]
        enums.append((name, values))

    # Parse structs
    for m in re.finditer(r"struct\s+(\w+)\s*\{([^}]*)\}", text):
        name = m.group(1)
        body = m.group(2).strip()
        fields = []
        for line in body.split(";"):
            line = line.strip()
            if not line:
                continue
            # Match: Type name[N][M] or Type name[N] or Type name
            fm = re.match(
                r"(\w+)\s+(\w+)((?:\[\w+\])*)\s*$", line
            )
            if fm:
                ctype, fname, dims = fm.group(1), fm.group(2), fm.group(3)
                dim_list = re.findall(r"\[(\w+)\]", dims)
                fields.append((ctype, fname, dim_list))
        structs.append((name, fields))

    # Parse API_COMMAND comments
    for m in re.finditer(
        r"//\s*API_COMMAND\((\w+),\s*(\w+),\s*([^,]+),\s*(\w+),\s*(\w+)\)",
        text,
    ):
        commands.append({
            "name": m.group(1),
            "method": m.group(2),
            "path": m.group(3).strip(),
            "request": m.group(4),
            "response": m.group(5),
        })

    return enums, structs, commands


def ts_type(ctype: str, dims: list, enum_names: set, struct_names: set) -> str:
    if ctype in enum_names:
        base = ctype
    elif ctype in struct_names:
        base = ctype
    elif ctype in TYPE_MAP:
        base = TYPE_MAP[ctype]
    else:
        base = "any"
    for _ in dims:
        base += "[]"
    return base


def to_camel_func(snake: str) -> str:
    """Convert snake_case to camelCase for function names only."""
    parts = snake.split("_")
    return parts[0] + "".join(p.capitalize() for p in parts[1:])


def generate_ts(enums, structs, commands) -> str:
    lines = [
        "// AUTO-GENERATED from firmware/api.h — do not edit",
        '// Run: python codegen/generate.py',
        "",
        'import { transport } from "../transport";',
        "",
    ]

    enum_names = {e[0] for e in enums}
    struct_names = {s[0] for s in structs}

    # Enums as string unions
    for name, values in enums:
        vals = " | ".join(f'"{v}"' for v in values)
        lines.append(f"export type {name} = {vals};")
    if enums:
        lines.append("")

    # Structs as interfaces
    for name, fields in structs:
        if not fields:
            lines.append(f"export type {name} = Record<string, never>;")
        else:
            lines.append(f"export interface {name} {{")
            for ctype, fname, dims in fields:
                ts = ts_type(ctype, dims, enum_names, struct_names)
                lines.append(f"  {fname}: {ts};")
            lines.append("}")
        lines.append("")

    # Command registry
    lines.append("export const commands = {")
    for cmd in commands:
        lines.append(
            f'  {cmd["name"]}: '
            f'{{ method: "{cmd["method"]}", path: "{cmd["path"]}" }},'
        )
    lines.append("} as const;")
    lines.append("")

    # Client functions
    for cmd in commands:
        fn = to_camel_func(cmd["name"])
        req = cmd["request"]
        res = cmd["response"]
        has_fields = any(
            len(fields) > 0
            for sname, fields in structs
            if sname == req
        )
        if has_fields:
            lines.append(
                f"export function {fn}(params: {req}): Promise<{res}> {{"
            )
            lines.append(
                f'  return transport.call("{cmd["name"]}", params);'
            )
        else:
            lines.append(
                f"export function {fn}(): Promise<{res}> {{"
            )
            lines.append(
                f'  return transport.call("{cmd["name"]}", {{}});'
            )
        lines.append("}")
        lines.append("")

    return "\n".join(lines)


def main():
    if not API_H.exists():
        print(f"Error: {API_H} not found", file=sys.stderr)
        sys.exit(1)

    text = API_H.read_text()
    enums, structs, commands = parse_api_header(text)

    ts = generate_ts(enums, structs, commands)
    OUT_TS.parent.mkdir(parents=True, exist_ok=True)
    OUT_TS.write_text(ts)
    print(f"Generated {OUT_TS.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
