"""Read-only checks for the two learning files and their local evidence binding.

This checks packaging and provenance, not the competition's Prompt score.
"""
from pathlib import Path
import hashlib
import json
import re

ROOT = Path(__file__).resolve().parents[1]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def pairs_unique(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'Duplicate JSON key: ' + key)
        result[key] = value
    return result


def is_within(path, directory):
    try:
        path.relative_to(directory)
        return True
    except ValueError:
        return False


def path_in_repo(name):
    path = (ROOT / name).resolve()
    require(is_within(path, ROOT), 'Reference leaves repository: ' + name)
    require(path.is_file(), 'Missing file: ' + name)
    return path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=pairs_unique)


def main():
    binding = read_json(path_in_repo('agent/material-digest.json'))
    require(binding['kind'] == 'local-material-integrity-manifest', 'Wrong integrity manifest')
    files = binding['material_files']
    require(set(files) == {'conversation.json', 'agent/conversation.json'}, 'Wrong material files')
    material_bytes = path_in_repo('conversation.json').read_bytes()
    require(material_bytes == path_in_repo('agent/conversation.json').read_bytes(), 'Material mirrors differ')
    require(material_bytes.startswith(b'{') and len(material_bytes) < 128 * 1024, 'Material encoding/size')
    for name, expected in files.items():
        path = path_in_repo(name)
        require(path.stat().st_size == expected['bytes'] and sha(path) == expected['sha256'], name + ' digest')
    current = read_json(path_in_repo('conversation.json'))
    history = binding['before_material']
    previous_path = path_in_repo(history['path'])
    require(sha(previous_path) == history['sha256'] and previous_path.stat().st_size == history['bytes'], 'History digest')
    previous = read_json(previous_path)
    for field in ['experiments', 'agent_observations']:
        require(current[field] == previous[field], 'Changed historical records: ' + field)
    count = len(previous['conversations'])
    require(current['conversations'][:count] == previous['conversations'], 'Changed user quotations')
    expected_new = [('u36', '这版的primpt怎么样'), ('u37', '按你的理解优化prompt'), ('u38', '继续')]
    actual_new = [(row['id'], row['content']) for row in current['conversations'][count:]]
    require(actual_new == expected_new, 'Unexpected additional user quotations')
    for field in ['conversations', 'experiments', 'agent_observations']:
        ids = [row['id'] for row in current[field]]
        require(len(ids) == len(set(ids)), 'Duplicate record IDs: ' + field)
    counts = binding['counts']
    require(counts == {'user_records':len(current['conversations']),
                       'experiments':len(current['experiments']),
                       'agent_observations':len(current['agent_observations']),
                       'retrospective_cases':len(current['case_studies'])}, 'Incorrect record counts')
    require('2026-11-11' in current['scope'], 'Stale coverage date')
    require(current['future_task_prompt']['source'] == 'agent_authored' and
            current['future_task_prompt']['status'] == 'new_template_not_executed', 'Template attribution')
    for section in ['material_overview', 'prompt_requirement_evolution']:
        require(current[section]['source'] == 'agent_retrospective', 'Retrospective attribution')
    for case in current['case_studies']:
        require(case['source'] == 'agent_retrospective', 'Case attribution')
        for name in case['files']:
            path_in_repo(name)

    reference_count = 0
    def check_references(value):
        nonlocal reference_count
        if isinstance(value, dict):
            if 'pointer' in value:
                pointer = value['pointer']
                require(pointer.startswith('/'), 'Invalid JSON pointer')
                target = current
                parent = None
                for part in pointer[1:].split('/'):
                    part = part.replace('~1', '/').replace('~0', '~')
                    parent = target
                    target = target[int(part)] if isinstance(target, list) else target[part]
                if 'id' in value:
                    require(parent['id'] == value['id'], 'Record ID/pointer mismatch: ' + pointer)
                if 'quote' in value:
                    require(isinstance(target, str) and value['quote'] in target, 'Quotation mismatch: ' + pointer)
                reference_count += 1
            for member in value.values():
                check_references(member)
        elif isinstance(value, list):
            for member in value:
                check_references(member)
    check_references(current)
    latest = current['review_navigation']['items'][-1]
    require([r['id'] for r in latest['result_evidence']] == [f'e{i}' for i in range(57,63)], 'Incomplete latest evidence chain')

    measurement = binding['measurement_evidence']
    manifest_path = path_in_repo(measurement['path'])
    require(sha(manifest_path) == measurement['sha256'], 'Historical measurement manifest changed')
    manifest = read_json(manifest_path)
    require(manifest_path.read_bytes() == path_in_repo('bench/results/prompt-material-20261111/measurement-manifest-before.json').read_bytes(), 'Historical manifest snapshot differs')
    require(manifest['learning_sha256'] == history['sha256'] == measurement['material_at_measurement_sha256'], 'Historical material binding differs')
    for name, expected in manifest['source_sha256'].items():
        require(sha(path_in_repo(name)) == expected, 'Measured source changed: ' + name)
    for name, expected in manifest['files'].items():
        path = manifest_path.parent / name
        require(is_within(path.resolve(), manifest_path.parent.resolve()), 'Invalid evidence path')
        require(sha(path) == expected, 'Historical evidence changed: ' + name)
    for content in [material_bytes.decode('utf-8'), previous_path.read_text(encoding='utf-8')]:
        require('github_pat_' not in content and 'ghp_' not in content, 'Credential marker in material')
        require(re.search(r'https?://[^/\s:]+:[^/\s@]+@', content) is None, 'Credential URL in material')
    print(json.dumps({'passed':True, 'bytes':len(material_bytes), 'sha256':sha(path_in_repo('conversation.json')),
                      'reference_checks':reference_count, 'preserved_user_records':count,
                      'preserved_experiments':len(previous['experiments']),
                      'source_hashes':len(manifest['source_sha256']), 'evidence_hashes':len(manifest['files']),
                      'platform_evaluation':'not performed'}, ensure_ascii=False))


if __name__ == '__main__':
    main()
