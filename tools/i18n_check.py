#!/usr/bin/env python3
# Copyright 漏 2026 SculkCatalystMC. All rights reserved.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, version 3 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

from __future__ import annotations

import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LANG_DIR = ROOT / "lang"
BASE_LOCALE = "en-US"


def strip_jsonc_comments(text: str) -> str:
    if text.startswith("\ufeff"):
        text = text[1:]
    output: list[str] = []
    in_string = False
    escaped = False
    i = 0
    while i < len(text):
        ch = text[i]
        if in_string:
            output.append(ch)
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                in_string = False
            i += 1
            continue

        if ch == '"':
            in_string = True
            output.append(ch)
            i += 1
            continue

        if ch == "/" and i + 1 < len(text) and text[i + 1] == "/":
            i += 2
            while i < len(text) and text[i] != "\n":
                i += 1
            if i < len(text):
                output.append("\n")
                i += 1
            continue

        if ch == "/" and i + 1 < len(text) and text[i + 1] == "*":
            i += 2
            while i + 1 < len(text) and not (text[i] == "*" and text[i + 1] == "/"):
                if text[i] == "\n":
                    output.append("\n")
                i += 1
            i += 2 if i + 1 < len(text) else 0
            continue

        output.append(ch)
        i += 1
    return "".join(output)


def load_catalog(path: Path) -> dict[str, object]:
    return json.loads(strip_jsonc_comments(path.read_text(encoding="utf-8")))


def messages(catalog: dict[str, object], path: Path) -> dict[str, str]:
    raw = catalog.get("messages")
    if not isinstance(raw, dict):
        raise ValueError(f"{path}: messages must be an object")

    result: dict[str, str] = {}
    for key, value in raw.items():
        if not isinstance(key, str):
            raise ValueError(f"{path}: message key must be a string")
        if not isinstance(value, str):
            raise ValueError(f"{path}: {key} value must be a string")
        result[key] = value
    return result


def extract_placeholders(value: str) -> tuple[set[str], list[str]]:
    placeholders: set[str] = set()
    errors: list[str] = []
    i = 0
    while i < len(value):
        ch = value[i]
        if ch == "{":
            if i + 1 < len(value) and value[i + 1] == "{":
                i += 2
                continue
            close = value.find("}", i + 1)
            if close < 0:
                errors.append("invalid brace")
                break
            inside = value[i + 1 : close]
            if inside == "":
                errors.append("bare {} placeholder")
            else:
                match = re.fullmatch(r"([0-9]+)(:.*)?", inside)
                if match is None:
                    errors.append(f"invalid placeholder {{{inside}}}")
                else:
                    placeholders.add(match.group(1))
            i = close + 1
            continue
        if ch == "}":
            if i + 1 < len(value) and value[i + 1] == "}":
                i += 2
                continue
            errors.append("invalid brace")
        i += 1
    return placeholders, errors


def check_catalog(
    label: str,
    current: dict[str, str],
    base: dict[str, str],
    *,
    strict: bool,
) -> bool:
    ok = True
    prefix = "ERROR" if strict else "WARN"

    for key in sorted(base):
        if key not in current:
            print(f"[i18n] {prefix}: {label} missing key: {key}")
            ok = False
            continue
        if current[key] == "":
            print(f"[i18n] {prefix}: {label} empty value: {key}")
            ok = False

        expected, expected_errors = extract_placeholders(base[key])
        actual, actual_errors = extract_placeholders(current[key])
        for err in actual_errors:
            print(f"[i18n] {prefix}: {label} {key}: {err}")
            ok = False
        for err in expected_errors:
            print(f"[i18n] ERROR: {BASE_LOCALE} {key}: {err}")
            ok = False
        if not expected_errors and not actual_errors and expected != actual:
            exp = ",".join(f"{{{item}}}" for item in sorted(expected)) or "<none>"
            got = ",".join(f"{{{item}}}" for item in sorted(actual)) or "<none>"
            print(f"[i18n] {prefix}: {label} placeholder mismatch: {key} expected {exp}, got {got}")
            ok = False

    for key in sorted(set(current) - set(base)):
        print(f"[i18n] {prefix}: {label} extra key: {key}")
        ok = False

    if ok:
        print(f"[i18n] OK: {label}")
    return ok or not strict


def main() -> int:
    base_path = LANG_DIR / f"{BASE_LOCALE}.jsonc"
    print(f"[i18n] base locale: {BASE_LOCALE}")
    try:
        base = messages(load_catalog(base_path), base_path)
    except Exception as exc:
        print(f"[i18n] ERROR: failed to read base catalog {base_path}: {exc}")
        return 1

    ok = True
    for path in sorted(LANG_DIR.glob("*.jsonc")):
        if path.name == "manifest.jsonc" or path.name == f"{BASE_LOCALE}.jsonc":
            continue
        label = path.stem
        print(f"[i18n] checking {label}")
        try:
            current = messages(load_catalog(path), path)
        except Exception as exc:
            print(f"[i18n] ERROR: failed to read {path}: {exc}")
            ok = False
            continue
        ok = check_catalog(label, current, base, strict=True) and ok

    user_dir = LANG_DIR / "user"
    if user_dir.exists():
        for path in sorted(user_dir.glob("*.jsonc")):
            if path.name.endswith(".missing.jsonc"):
                continue
            label = f"user/{path.stem}"
            print(f"[i18n] checking {label}")
            try:
                current = messages(load_catalog(path), path)
            except Exception as exc:
                print(f"[i18n] WARN: failed to read {path}: {exc}")
                continue
            check_catalog(label, current, base, strict=False)

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
