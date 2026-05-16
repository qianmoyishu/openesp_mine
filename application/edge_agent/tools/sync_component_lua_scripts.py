#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path


class LuaScriptSyncError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description='Sync component Lua scripts into the app FATFS image.'
    )
    parser.add_argument('--output-dir', required=True)
    parser.add_argument('--manifest-path', required=True)
    parser.add_argument(
        '--component',
        action='append',
        default=[],
        help='Component mapping in the form <name>=<dir>.',
    )
    parser.add_argument(
        '--main-dir',
        help='Optional app main directory whose lua_scripts should be treated as builtin scripts.',
    )
    return parser.parse_args()


def fail(message: str) -> None:
    raise LuaScriptSyncError(message)


def load_json_file(path: Path) -> object:
    try:
        return json.loads(path.read_text(encoding='utf-8'))
    except FileNotFoundError:
        fail(f'Missing JSON file: {path}')
    except json.JSONDecodeError as exc:
        fail(f'Invalid JSON in {path}: {exc}')


def load_manifest(path: Path) -> dict:
    if not path.exists():
        return {'synced_files': [], 'sources': {}}

    data = load_json_file(path)
    if not isinstance(data, dict):
        fail(f'Manifest must be a JSON object: {path}')

    synced_files = data.get('synced_files', [])
    sources = data.get('sources', {})
    if not isinstance(synced_files, list):
        fail(f"Manifest field 'synced_files' must be a JSON array: {path}")
    if not isinstance(sources, dict):
        fail(f"Manifest field 'sources' must be a JSON object: {path}")

    return {
        'synced_files': [str(item) for item in synced_files],
        'sources': {str(key): str(value) for key, value in sources.items()},
    }


def parse_component_specs(raw_specs: list[str]) -> list[tuple[str, Path]]:
    components: list[tuple[str, Path]] = []

    for spec in raw_specs:
        if '=' not in spec:
            fail(f"Invalid component mapping '{spec}', expected <name>=<dir>.")
        name, raw_dir = spec.split('=', 1)
        if not name or not raw_dir:
            fail(f"Invalid component mapping '{spec}', expected <name>=<dir>.")
        components.append((name, Path(raw_dir).resolve()))

    components.sort(key=lambda item: item[0])
    return components


def collect_builtin_lua_scripts(
    components: list[tuple[str, Path]],
    main_dir: Path | None,
) -> tuple[dict[str, Path], dict[str, str]]:
    copy_map: dict[str, Path] = {}
    source_map: dict[str, str] = {}

    source_specs = list(components)
    if main_dir is not None:
        source_specs.append(('main', main_dir.resolve()))

    for source_name, source_root in source_specs:
        scripts_dir = source_root / 'lua_scripts'
        if not scripts_dir.is_dir():
            continue

        for script_path in sorted(scripts_dir.glob('*.lua')):
            if not script_path.is_file():
                continue

            script_name = script_path.name
            previous_source = source_map.get(script_name)
            if previous_source:
                fail(
                    f"Duplicate builtin Lua script '{script_name}' between {previous_source} "
                    f"and {source_name} ({script_path})"
                )

            resolved_path = script_path.resolve()
            copy_map[script_name] = resolved_path
            source_map[script_name] = f'{source_name} ({resolved_path})'

    return copy_map, source_map


def sync_builtin_lua_scripts(
    output_dir: Path,
    manifest_path: Path,
    manifest: dict,
    copy_map: dict[str, Path],
    source_map: dict[str, str],
) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)

    for old_file in manifest.get('synced_files', []):
        if old_file not in copy_map:
            stale_path = output_dir / old_file
            if stale_path.exists():
                stale_path.unlink()

    for filename, source_path in copy_map.items():
        target_path = output_dir / filename
        shutil.copy2(source_path, target_path)

    manifest_data = {
        'synced_files': sorted(copy_map.keys()),
        'sources': {filename: source_map[filename] for filename in sorted(source_map)},
    }
    manifest_path.write_text(
        json.dumps(manifest_data, ensure_ascii=False, indent=2) + '\n',
        encoding='utf-8',
    )


def main() -> int:
    args = parse_args()

    output_dir = Path(args.output_dir).resolve()
    manifest_path = Path(args.manifest_path).resolve()
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest = load_manifest(manifest_path)

    components = parse_component_specs(args.component)
    main_dir = Path(args.main_dir).resolve() if args.main_dir else None
    copy_map, source_map = collect_builtin_lua_scripts(components, main_dir)
    sync_builtin_lua_scripts(output_dir, manifest_path, manifest, copy_map, source_map)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except LuaScriptSyncError as exc:
        print(f'sync_component_lua_scripts.py: error: {exc}', file=sys.stderr)
        sys.exit(1)
