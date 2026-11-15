"""Validate the v3 curated material and its exact archived string provenance.

This is local integrity checking, not an official Prompt evaluation.
"""
import hashlib
import json
import re
from pathlib import Path


def verify(root, helpers):
    need = helpers.require
    binding = helpers.read_json(root/'agent/material-digest.json')
    raw = (root/'agent/conversation.json').read_bytes()
    need(raw == (root/'conversation.json').read_bytes(), 'Material mirrors differ')
    need(0 < len(raw) <= 131072, 'Material exceeds byte limit')
    current = helpers.read_json(root/'agent/conversation.json')
    need(current['schema_version'] == 'minivectordb-evidence-and-implementation-v3', 'Wrong schema')
    need(current['rule_version'] == 'agent-learning-evidence-v3-unbounded', 'Wrong rule')
    need(current['material_type'] == 'real' and current['synthetic'] is False, 'Wrong material attribution')
    need(current['summary'] and current['material_summary'] and current['scope'], 'Missing scope/summary')
    need(binding['kind'] == 'local-material-integrity-manifest', 'Wrong manifest')
    need(set(binding['material_files']) == {'agent/conversation.json','conversation.json'}, 'Wrong entry list')
    def check_file(item):
        p = (root/item['path']).resolve()
        need(helpers.is_within(p,root.resolve()) and p.is_file(), 'Invalid bound file')
        data = p.read_bytes()
        need(len(data) == item['bytes'] and hashlib.sha256(data).hexdigest() == item['sha256'], 'Bound file changed: '+item['path'])
        return data
    for name, metadata in binding['material_files'].items():
        check_file(dict(metadata,path=name))
    history = {name:{} for name in ['conversations','experiments','agent_observations']}
    for archive in binding['record_sources']:
        check_file(archive)
        data = helpers.read_json(root/archive['path'])
        for name in history:
            for record in data.get(name,[]):
                need(record['id'] not in history[name], 'Duplicate source record: '+record['id'])
                history[name][record['id']] = record
    for kind, field in [('conversations','content'),('experiments','output')]:
        seen = set()
        for record in current[kind]:
            identity = record['id']
            need(identity not in seen, 'Duplicate selected record')
            seen.add(identity)
            need(identity in history[kind], 'Unbound source record: '+identity)
            original = history[kind][identity]
            need(record[field] == original[field], 'Changed historical string: '+identity)
            if kind == 'conversations':
                original_role = original.get('role', original.get('source'))
                need(original_role in ('user','assistant') and record['role'] == original_role, 'Changed attribution: '+identity)
                if record['source'] == 'current_conversation':
                    need(record == original, 'Changed current conversation context: '+identity)
            elif record['source'] != 'historical_experiment_summary':
                need(record == original, 'Changed current result context: '+identity)
    expected_counts = {k:len(current[k]) for k in ['conversations','experiments','evidence_chains']}
    need(current['record_counts'] == expected_counts == binding['counts'], 'Incorrect record counts')
    reference_count = helpers.check_references(current)
    for kind in ['student_evidence','result_evidence']:
        seen = set()
        for chain in current['evidence_chains']:
            for ref in chain[kind]:
                need(ref['id'] not in seen, 'Repeated evidence owner')
                seen.add(ref['id'])
                if kind == 'student_evidence':
                    _, parent = helpers.resolve_pointer(current,ref['pointer'])
                    need(parent['role'] == 'user', 'Agent material used as student evidence')
    need(current['source_identity'] == binding['current_source_identity'], 'Runtime identity mismatch')
    spec = current['implementation_spec']
    need(spec['source'] == 'agent_retrospective_specification', 'Incorrect specification attribution')
    need(spec['target_engine_commit'] == current['source_identity']['verified_engine_commit'], 'Spec target mismatch')
    for path, field in binding['specification_files'].items():
        need((root/path).read_text() == spec[field], 'Specification companion mismatch: '+path)
    combined = '\n'.join(spec[k] for k in binding['combined_prompt_fields'])
    need((root/'agent/implementation-prompt.txt').read_text() == combined, 'Combined prompt mismatch')
    for path, expected in binding['runtime_blob_sha1'].items():
        data = (root/path).read_bytes()
        actual = hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
        need(actual == expected, 'Runtime blob changed: '+path)
    need(not re.search(r'github_pat_|gh[pousr]_[A-Za-z0-9]{20,}|sk-sp-|sk-[A-Za-z0-9_-]{20,}|https?://[^\s/"<>]+:[^\s/@"<>]+@', raw.decode()), 'Credential marker in material')
    print(json.dumps({'passed':True,'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest(),
                      'references':reference_count,'counts':expected_counts,
                      'runtime_blobs':len(binding['runtime_blob_sha1']),
                      'official_score':None,'scope':'integrity and provenance only'},ensure_ascii=False))
