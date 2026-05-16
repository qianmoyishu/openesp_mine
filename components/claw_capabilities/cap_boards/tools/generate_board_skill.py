#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Any

PIN_KEY_SUFFIXES = (
    '_io_num',
    '_gpio_num',
    '_io',
)
PIN_KEY_NAMES = {
    'gpio_num',
    'sda',
    'scl',
    'mclk',
    'bclk',
    'ws',
    'mosi',
    'miso',
    'sclk',
    'clk',
    'dout',
    'din',
    'cs',
    'dc',
    'reset',
    'vsync',
    'hsync',
    'de',
    'pclk',
    'xclk',
    'pwdn',
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description='Generate current board skill markdown.')
    parser.add_argument('--gen-bmgr-dir', required=True, help='Path to application/edge_agent/components/gen_bmgr_codes')
    parser.add_argument('--output-md', required=True, help='Generated markdown output path')
    return parser.parse_args()


def fail(message: str) -> None:
    raise RuntimeError(message)


def parse_board_path(gen_bmgr_dir: Path) -> Path:
    cmake_path = gen_bmgr_dir / 'CMakeLists.txt'
    text = cmake_path.read_text(encoding='utf-8')
    match = re.search(r'message\(STATUS "Board Path: ([^"]+)"\)', text)
    if not match:
        fail(f'Could not resolve board path from {cmake_path}')
    return Path(match.group(1)).resolve()


def strip_c_comments(text: str) -> str:
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.DOTALL)
    return re.sub(r'//.*', '', text)


class CInitializerParser:
    def __init__(self, text: str) -> None:
        self.text = text
        self.length = len(text)
        self.index = 0

    def parse(self) -> Any:
        self._skip_ws()
        value = self._parse_value()
        self._skip_ws()
        return value

    def _skip_ws(self) -> None:
        while self.index < self.length and self.text[self.index].isspace():
            self.index += 1

    def _peek(self) -> str | None:
        if self.index >= self.length:
            return None
        return self.text[self.index]

    def _consume(self, expected: str | None = None) -> str:
        char = self._peek()
        if char is None:
            fail('Unexpected end of C initializer')
        if expected is not None and char != expected:
            fail(f'Expected {expected!r} in C initializer, got {char!r}')
        self.index += 1
        return char

    def _parse_value(self) -> Any:
        self._skip_ws()
        char = self._peek()
        if char == '{':
            return self._parse_initializer()
        if char == '"':
            return self._parse_string()
        if char == '-':
            self._consume('-')
            self._skip_ws()
            number = self._parse_number()
            if isinstance(number, int):
                return -number
            return f'-{number}'
        if char is None:
            fail('Missing C initializer value')
        if char.isdigit():
            return self._parse_number()
        return self._parse_expression()

    def _parse_initializer(self) -> Any:
        self._consume('{')
        self._skip_ws()
        if self._peek() == '}':
            self._consume('}')
            return []

        items: list[Any] = []
        mapping: dict[str, Any] = {}
        has_designators = False

        while True:
            self._skip_ws()
            char = self._peek()
            if char == '.':
                has_designators = True
                key = self._parse_designator()
                self._skip_ws()
                self._consume('=')
                mapping[key] = self._parse_value()
            else:
                items.append(self._parse_value())

            self._skip_ws()
            char = self._peek()
            if char == ',':
                self._consume(',')
                self._skip_ws()
                if self._peek() == '}':
                    self._consume('}')
                    break
                continue
            if char == '}':
                self._consume('}')
                break
            fail(f'Unexpected token {char!r} inside C initializer')

        return mapping if has_designators else items

    def _parse_designator(self) -> str:
        self._consume('.')
        start = self.index
        while self.index < self.length and (self.text[self.index].isalnum() or self.text[self.index] == '_'):
            self.index += 1
        if start == self.index:
            fail('Expected designator name in C initializer')
        return self.text[start:self.index]

    def _parse_string(self) -> str:
        self._consume('"')
        result: list[str] = []
        while True:
            char = self._consume()
            if char == '"':
                return ''.join(result)
            if char == '\\':
                escaped = self._consume()
                translations = {'n': '\n', 'r': '\r', 't': '\t', '\\': '\\', '"': '"'}
                result.append(translations.get(escaped, escaped))
                continue
            result.append(char)

    def _parse_number(self) -> int | float | str:
        start = self.index
        while self.index < self.length and (self.text[self.index].isalnum() or self.text[self.index] in {'.', 'x', 'X'}):
            self.index += 1
        token = self.text[start:self.index]
        if any(marker in token for marker in {'.', 'e', 'E'}):
            try:
                return float(token)
            except ValueError:
                return token
        try:
            return int(token, 0)
        except ValueError:
            return token

    def _parse_expression(self) -> Any:
        start = self.index
        paren_depth = 0
        while self.index < self.length:
            char = self.text[self.index]
            if char == '(':
                paren_depth += 1
            elif char == ')':
                if paren_depth == 0:
                    break
                paren_depth -= 1
            elif paren_depth == 0 and char in {',', '}'}:
                break
            self.index += 1
        token = self.text[start:self.index].strip()
        if token == 'true':
            return True
        if token == 'false':
            return False
        if token == 'NULL':
            return None
        return token


def find_initializer_block(text: str, variable_name: str) -> str:
    pattern = re.compile(rf'^\s*.*?\b{re.escape(variable_name)}\b\s*(?:\[[^\]]*\])?\s*=\s*\{{', re.MULTILINE)
    match = pattern.search(text)
    if not match:
        fail(f'Could not find initializer for {variable_name}')

    brace_start = text.find('{', match.start())
    depth = 0
    index = brace_start
    while index < len(text):
        char = text[index]
        if char == '{':
            depth += 1
        elif char == '}':
            depth -= 1
            if depth == 0:
                return text[brace_start:index + 1]
        index += 1
    fail(f'Unterminated initializer for {variable_name}')


def parse_c_initializer_file(path: Path, variable_name: str) -> Any:
    try:
        text = path.read_text(encoding='utf-8')
    except FileNotFoundError as exc:
        raise RuntimeError(f'Missing generated source file: {path}') from exc
    initializer = find_initializer_block(strip_c_comments(text), variable_name)
    return CInitializerParser(initializer).parse()


def load_board_info(gen_bmgr_dir: Path) -> dict[str, Any]:
    data = parse_c_initializer_file(gen_bmgr_dir / 'gen_board_info.c', 'g_esp_board_info')
    if not isinstance(data, dict):
        fail('Expected g_esp_board_info to be a designated initializer')
    return {
        'board': data.get('name', 'unknown'),
        'chip': data.get('chip', 'unknown'),
        'version': data.get('version', 'unknown'),
        'description': data.get('description', ''),
        'manufacturer': data.get('manufacturer', 'unknown'),
    }


def load_peripheral_map(gen_bmgr_dir: Path) -> dict[str, dict[str, Any]]:
    source = gen_bmgr_dir / 'gen_board_periph_config.c'
    descriptors = parse_c_initializer_file(source, 'g_esp_board_peripherals')
    if not isinstance(descriptors, list):
        fail('Expected g_esp_board_peripherals to be an array initializer')

    peripheral_map: dict[str, dict[str, Any]] = {}
    for descriptor in descriptors:
        if not isinstance(descriptor, dict):
            continue
        name = descriptor.get('name')
        cfg_ref = descriptor.get('cfg')
        if not isinstance(name, str) or not name or not isinstance(cfg_ref, str) or not cfg_ref.startswith('&'):
            continue
        cfg_var_name = cfg_ref[1:]
        cfg = parse_c_initializer_file(source, cfg_var_name)
        peripheral_map[name] = summarize_peripheral({'name': name, 'config': cfg})
    return peripheral_map


def collect_peripheral_refs(node: Any, parents: list[str] | None = None) -> list[str]:
    parents = parents or []
    refs: list[str] = []

    if isinstance(node, dict):
        for key, value in node.items():
            if isinstance(value, str):
                if key in {'gpio_name', 'ledc_name', 'i2c_name', 'spi_name', 'periph_name', 'peripheral_name'}:
                    refs.append(value)
                elif key == 'name' and parents and parents[-1] in {'pa_cfg', 'i2c_cfg', 'i2s_cfg'}:
                    refs.append(value)
            refs.extend(collect_peripheral_refs(value, parents + [str(key)]))
    elif isinstance(node, list):
        for item in node:
            refs.extend(collect_peripheral_refs(item, parents))

    return refs


def load_devices(gen_bmgr_dir: Path, peripheral_map: dict[str, dict[str, Any]]) -> list[dict[str, Any]]:
    source = gen_bmgr_dir / 'gen_board_device_config.c'
    descriptors = parse_c_initializer_file(source, 'g_esp_board_devices')
    if not isinstance(descriptors, list):
        fail('Expected g_esp_board_devices to be an array initializer')

    devices: list[dict[str, Any]] = []
    for descriptor in descriptors:
        if not isinstance(descriptor, dict):
            continue
        name = descriptor.get('name')
        cfg_ref = descriptor.get('cfg')
        if not isinstance(name, str) or not name or name.startswith('fake'):
            continue
        if not isinstance(cfg_ref, str) or not cfg_ref.startswith('&'):
            continue

        config = parse_c_initializer_file(source, cfg_ref[1:])
        device = summarize_device({'name': name, 'config': config}, peripheral_map)

        power_ctrl_device = descriptor.get('power_ctrl_device')
        if isinstance(power_ctrl_device, str) and power_ctrl_device:
            related_device = next((item for item in devices if item['name'] == power_ctrl_device), None)
            if related_device:
                device['peripherals'].append({'name': power_ctrl_device, 'io_lines': related_device['io_lines']})

        devices.append(device)
    return devices


def normalize_label(path_parts: list[str]) -> str:
    filtered = [part for part in path_parts if part not in {'config', 'pins', 'flags', 'sub_cfg', 'lcd_panel_config', 'io_spi_config', 'spi_bus_config'}]
    if not filtered:
        filtered = path_parts
    normalized_parts = []
    for part in filtered:
        normalized = part.replace('_io_num', '').replace('_gpio_num', '').replace('_io', '')
        if normalized == 'gpio_num':
            normalized = 'gpio'
        normalized_parts.append(normalized)
    label = '.'.join(part for part in normalized_parts if part)
    if label == 'doubt':
        return 'dout'
    return label


def is_pin_key(key: str, parents: list[str]) -> bool:
    if parents and parents[-1] == 'levels':
        return False
    return key in PIN_KEY_NAMES or key.endswith(PIN_KEY_SUFFIXES) or (parents and parents[-1] == 'pins')


def collect_io_entries(node: Any, path_parts: list[str] | None = None) -> list[tuple[str, int]]:
    path_parts = path_parts or []
    entries: list[tuple[str, int]] = []

    if isinstance(node, dict):
        for key, value in node.items():
            current_path = path_parts + [str(key)]
            if key == 'pin_bit_mask' and isinstance(value, str):
                match = re.fullmatch(r'BIT(?:64)?\((\d+)\)', value)
                if match:
                    entries.append(('gpio', int(match.group(1))))
                continue
            if isinstance(value, int) and value >= 0 and is_pin_key(str(key), path_parts):
                entries.append((normalize_label(current_path), value))
                continue
            if isinstance(value, list) and key == 'data_io':
                for index, item in enumerate(value):
                    if isinstance(item, int) and item >= 0:
                        entries.append((f'{normalize_label(current_path)}[{index}]', item))
                continue
            entries.extend(collect_io_entries(value, current_path))
        return entries

    if isinstance(node, list):
        for index, item in enumerate(node):
            entries.extend(collect_io_entries(item, path_parts + [str(index)]))
    return entries


def format_io_list(entries: list[tuple[str, int]]) -> list[str]:
    seen: set[tuple[str, int]] = set()
    lines: list[str] = []
    for label, pin in entries:
        key = (label, pin)
        if key in seen:
            continue
        seen.add(key)
        lines.append(f'- `{label}` -> `GPIO{pin}`')
    return lines


def summarize_peripheral(peripheral: dict[str, Any]) -> dict[str, Any]:
    config = peripheral.get('config')
    io_entries = collect_io_entries(config) if isinstance(config, dict) else []
    return {
        'name': str(peripheral.get('name', '')),
        'io_lines': format_io_list(io_entries),
    }


def summarize_device(device: dict[str, Any], peripheral_map: dict[str, dict[str, Any]]) -> dict[str, Any]:
    name = str(device.get('name', ''))
    config = device.get('config')
    io_entries = collect_io_entries(config) if isinstance(config, dict) else []
    peripherals: list[dict[str, Any]] = []

    for peripheral_name in dict.fromkeys(collect_peripheral_refs(config) if isinstance(config, dict) else []):
        periph_summary = peripheral_map.get(peripheral_name)
        peripherals.append({
            'name': peripheral_name,
            'io_lines': periph_summary['io_lines'] if periph_summary else [],
        })

    return {
        'name': name,
        'io_lines': format_io_list(io_entries),
        'peripherals': peripherals,
    }


def render_markdown(board_info: dict[str, Any], board_version: str, devices: list[dict[str, Any]]) -> str:
    board_name = str(board_info.get('board', 'unknown'))
    chip = str(board_info.get('chip', 'unknown'))
    manufacturer = str(board_info.get('manufacturer', 'unknown'))
    description = str(board_info.get('description', ''))
    lines = [
        f'# Current Board Hardware: {board_name}',
        '',
        'Read this skill before operating hardware, assigning GPIOs, or writing Lua and board-specific code.',
        '',
        '## Rules',
        '- Before operating any hardware, read this skill first.',
        '- Before assigning a GPIO, check whether it is already occupied below.',
        '- When writing Lua or board-specific code, use the listed device names instead of guessing hardware wiring.',
        '',
        '## Board Summary',
        f'- Board: `{board_name}`',
        f'- Chip: `{chip}`',
        f'- Version: `{board_version}`',
        f'- Manufacturer: `{manufacturer}`',
    ]

    if description:
        lines.append(f'- Description: {description}')

    lines.extend([
        '',
        '## Device Inventory',
    ])

    for device in devices:
        title = f"### {device['name']}"
        lines.extend(['', title])

        io_lines = list(device['io_lines'])
        for peripheral in device['peripherals']:
            io_lines.extend(peripheral['io_lines'])
        io_lines = list(dict.fromkeys(io_lines))

        if io_lines:
            lines.append('- Occupied IO:')
            lines.extend([f'  {line}' for line in io_lines])
        else:
            lines.append('- Occupied IO: none declared')

    lines.extend([
        '',
        '## Notes',
        '- If a device has no explicit IO mapping here, treat it as unknown instead of guessing.',
        '',
    ])
    return '\n'.join(lines)

def main() -> int:
    args = parse_args()
    gen_bmgr_dir = Path(args.gen_bmgr_dir).resolve()
    output_md = Path(args.output_md).resolve()

    board_dir = parse_board_path(gen_bmgr_dir)
    print(f'[cap_boards] Loading generated board metadata from {gen_bmgr_dir} (board path: {board_dir})')
    board_info = load_board_info(gen_bmgr_dir)
    peripheral_map = load_peripheral_map(gen_bmgr_dir)
    devices = load_devices(gen_bmgr_dir, peripheral_map)
    board_version = str(board_info.get('version') or 'unknown')
    markdown = render_markdown(board_info, board_version, devices)

    output_md.parent.mkdir(parents=True, exist_ok=True)
    output_md.write_text(markdown, encoding='utf-8')
    print(
        f"[cap_boards] Generated skill for board {board_info.get('board', 'unknown')} "
        f'with {len(devices)} devices: {output_md}'
    )
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except Exception as exc:  # noqa: BLE001
        print(f'generate_board_skill.py: {exc}', file=sys.stderr)
        raise SystemExit(1)
