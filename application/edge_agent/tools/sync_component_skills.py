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


SKILLS_LIST_FILE = 'skills_list.json'


class SkillSyncError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description='Sync component skills into the app FATFS image.'
    )
    parser.add_argument('--demo-skills-dir', required=True)
    parser.add_argument('--manifest-path', required=True)
    parser.add_argument(
        '--component',
        action='append',
        default=[],
        help='Component mapping in the form <name>=<dir>.',
    )
    parser.add_argument(
        '--exclude-skill-id',
        action='append',
        default=[],
        help='Skill id to exclude from the synced demo skill catalog.',
    )
    return parser.parse_args()


def fail(message: str) -> None:
    raise SkillSyncError(message)


def load_json_file(path: Path) -> object:
    try:
        return json.loads(path.read_text(encoding='utf-8'))
    except FileNotFoundError:
        fail(f'Missing JSON file: {path}')
    except json.JSONDecodeError as exc:
        fail(f'Invalid JSON in {path}: {exc}')


def load_manifest(path: Path) -> dict:
    if not path.exists():
        return {'component_entries': [], 'component_files': []}

    data = load_json_file(path)
    if not isinstance(data, dict):
        fail(f'Manifest must be a JSON object: {path}')
    return {
        'component_entries': list(data.get('component_entries', [])),
        'component_files': list(data.get('component_files', [])),
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


def build_component_entry_keys(entries: list[dict]) -> set[tuple[str, str]]:
    keys: set[tuple[str, str]] = set()
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        skill_id = entry.get('id')
        skill_file = entry.get('file')
        if isinstance(skill_id, str) and skill_id and isinstance(skill_file, str) and skill_file:
            keys.add((skill_id, skill_file))
    return keys


def recover_demo_entries(
    current_entries: list[dict],
    manifest: dict,
    component_entries: list[dict],
) -> list[dict]:
    component_entry_keys = build_component_entry_keys(list(manifest.get('component_entries', [])))
    component_entry_keys.update(build_component_entry_keys(component_entries))

    demo_entries: list[dict] = []
    for entry in current_entries:
        if not isinstance(entry, dict):
            fail('skills_list.json must contain only object entries in the skills array.')
        entry_key = (str(entry.get('id')), str(entry.get('file')))
        if entry_key in component_entry_keys:
            continue
        demo_entries.append(entry)

    return demo_entries


def load_demo_entries(skills_list_path: Path, manifest: dict, component_entries: list[dict]) -> list[dict]:
    if not skills_list_path.exists():
        return []

    data = load_json_file(skills_list_path)
    if not isinstance(data, dict):
        fail(f'{skills_list_path} must be a JSON object.')

    skills = data.get('skills')
    if not isinstance(skills, list):
        fail(f"{skills_list_path} must contain a 'skills' array.")

    return recover_demo_entries(skills, manifest, component_entries)


def collect_component_skills(
    components: list[tuple[str, Path]],
    excluded_skill_ids: set[str],
) -> tuple[list[dict], dict[str, Path], dict[tuple[str, str], str]]:
    component_entries: list[dict] = []
    copy_map: dict[str, Path] = {}
    skill_id_sources: dict[str, tuple[str, Path]] = {}
    skill_file_sources: dict[str, tuple[str, Path]] = {}
    component_entry_sources: dict[tuple[str, str], str] = {}

    for component_name, component_dir in components:
        skills_dir = component_dir / 'skills'
        if not skills_dir.is_dir():
            continue

        json_path = skills_dir / SKILLS_LIST_FILE
        if not json_path.exists():
            continue

        data = load_json_file(json_path)
        if not isinstance(data, dict):
            fail(f'{json_path} must be a JSON object.')

        skills = data.get('skills')
        if not isinstance(skills, list):
            fail(f"{json_path} must contain a 'skills' array.")

        for entry in skills:
            if not isinstance(entry, dict):
                fail(f"{json_path} must contain only object entries in 'skills'.")

            skill_id = entry.get('id')
            skill_file = entry.get('file')
            if not isinstance(skill_id, str) or not skill_id:
                fail(f"{json_path} contains a skill entry without a valid string 'id'.")
            if not isinstance(skill_file, str) or not skill_file:
                fail(f"{json_path} contains a skill entry without a valid string 'file'.")
            if skill_id in excluded_skill_ids:
                continue

            skill_path = (skills_dir / skill_file).resolve()
            if skill_path.parent != skills_dir.resolve():
                fail(
                    f"Component '{component_name}' declares skill file outside its skills directory: "
                    f'{skill_file} ({json_path})'
                )
            if not skill_path.is_file():
                fail(
                    f"Component '{component_name}' references missing skill file '{skill_file}' "
                    f'in {json_path}'
                )
            if skill_path.suffix.lower() != '.md':
                fail(
                    f"Component '{component_name}' declares non-markdown skill file '{skill_file}' "
                    f'in {json_path}'
                )

            previous_id = skill_id_sources.get(skill_id)
            if previous_id:
                fail(
                    f"Duplicate skill id '{skill_id}' between component '{previous_id[0]}' "
                    f"({previous_id[1]}) and component '{component_name}' ({json_path})"
                )
            previous_file = skill_file_sources.get(skill_file)
            if previous_file:
                fail(
                    f"Duplicate skill file '{skill_file}' between component '{previous_file[0]}' "
                    f"({previous_file[1]}) and component '{component_name}' ({json_path})"
                )
            if skill_file in copy_map:
                fail(
                    f"Duplicate markdown file '{skill_file}' between component sources "
                    f"'{copy_map[skill_file]}' and '{skill_path}'"
                )

            skill_id_sources[skill_id] = (component_name, json_path)
            skill_file_sources[skill_file] = (component_name, json_path)
            component_entry_sources[(skill_id, skill_file)] = f"component '{component_name}' ({json_path})"
            copy_map[skill_file] = skill_path
            component_entries.append(entry)

    return component_entries, copy_map, component_entry_sources


def merge_entries(
    demo_entries: list[dict],
    component_entries: list[dict],
    demo_skills_dir: Path,
    component_entry_sources: dict[tuple[str, str], str],
) -> list[dict]:
    merged_entries: list[dict] = []
    skill_ids: dict[str, str] = {}
    skill_files: dict[str, str] = {}

    def register(entry: dict, source: str) -> None:
        skill_id = entry.get('id')
        skill_file = entry.get('file')
        if not isinstance(skill_id, str) or not skill_id:
            fail(f"{source} contains a skill entry without a valid string 'id'.")
        if not isinstance(skill_file, str) or not skill_file:
            fail(f"{source} contains a skill entry without a valid string 'file'.")

        previous_source = skill_ids.get(skill_id)
        if previous_source:
            fail(f"Duplicate skill id '{skill_id}' between {previous_source} and {source}")

        previous_file_source = skill_files.get(skill_file)
        if previous_file_source:
            fail(f"Duplicate skill file '{skill_file}' between {previous_file_source} and {source}")

        skill_ids[skill_id] = source
        skill_files[skill_file] = source
        merged_entries.append(entry)

    for entry in demo_entries:
        skill_file = entry.get('file')
        register(entry, f'demo skills list ({demo_skills_dir / skill_file})')

    for entry in component_entries:
        entry_key = (str(entry.get('id')), str(entry.get('file')))
        register(entry, component_entry_sources.get(entry_key, 'component skills list'))

    return merged_entries


def validate_demo_markdown_files(demo_entries: list[dict], demo_skills_dir: Path) -> None:
    for entry in demo_entries:
        skill_file = entry.get('file')
        skill_path = demo_skills_dir / str(skill_file)
        if not skill_path.is_file():
            fail(
                f"Demo skills_list.json references missing skill file '{skill_file}' "
                f'at {skill_path}'
            )


def sync_markdown_files(
    demo_skills_dir: Path,
    manifest_path: Path,
    manifest: dict,
    copy_map: dict[str, Path],
    merged_entries: list[dict],
    component_entries: list[dict],
) -> None:
    demo_skills_dir.mkdir(parents=True, exist_ok=True)

    for old_file in manifest.get('component_files', []):
        if old_file not in copy_map:
            stale_path = demo_skills_dir / old_file
            if stale_path.exists():
                stale_path.unlink()

    for filename, source_path in copy_map.items():
        target_path = demo_skills_dir / filename
        shutil.copy2(source_path, target_path)

    skills_list_path = demo_skills_dir / SKILLS_LIST_FILE
    skills_list_path.write_text(
        json.dumps({'skills': merged_entries}, ensure_ascii=False, indent=2) + '\n',
        encoding='utf-8',
    )

    manifest_data = {
        'component_entries': component_entries,
        'component_files': sorted(copy_map.keys()),
    }
    manifest_path.write_text(
        json.dumps(manifest_data, ensure_ascii=False, indent=2) + '\n',
        encoding='utf-8',
    )


def main() -> int:
    args = parse_args()
    demo_skills_dir = Path(args.demo_skills_dir).resolve()

    skills_list_path = demo_skills_dir / SKILLS_LIST_FILE
    manifest_path = Path(args.manifest_path).resolve()
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest = load_manifest(manifest_path)

    components = parse_component_specs(args.component)
    excluded_skill_ids = {skill_id for skill_id in args.exclude_skill_id if skill_id}
    component_entries, copy_map, component_entry_sources = collect_component_skills(
        components,
        excluded_skill_ids,
    )
    demo_entries = load_demo_entries(skills_list_path, manifest, component_entries)
    validate_demo_markdown_files(demo_entries, demo_skills_dir)
    merged_entries = merge_entries(
        demo_entries,
        component_entries,
        demo_skills_dir,
        component_entry_sources,
    )

    sync_markdown_files(
        demo_skills_dir,
        manifest_path,
        manifest,
        copy_map,
        merged_entries,
        component_entries,
    )
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except SkillSyncError as exc:
        print(f'sync_component_skills.py: error: {exc}', file=sys.stderr)
        sys.exit(1)
