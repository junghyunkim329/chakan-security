"""Normal-only measured envelope; its boundaries are experimental policy."""
from collections import deque
from copy import deepcopy
import hashlib
import json
import re

from .model import BUSES, RULE_IDS, Frame

WINDOW_US = 1_000_000


def seal_policy(document):
    p = deepcopy(document)
    p.pop('policy_hash', None)
    encoded = json.dumps(p, sort_keys=True, separators=(',', ':'), ensure_ascii=True, allow_nan=False).encode()
    p['policy_hash'] = hashlib.sha256(encoded).hexdigest()
    return p


def validate_policy(document):
    if not isinstance(document, dict) or seal_policy(document)['policy_hash'] != document.get('policy_hash'):
        raise ValueError('policy hash mismatch')
    if set(document) != {'schema_version', 'kind', 'window_us', 'rule_ids', 'training', 'profiles', 'policy_hash'}:
        raise ValueError('unexpected policy fields')
    if type(document['schema_version']) is not int or document['schema_version'] != 1:
        raise ValueError('unsupported policy schema')
    if document['kind'] != 'normal_observed_envelope' or document['rule_ids'] != list(RULE_IDS):
        raise ValueError('unsupported policy kind or rules')
    if type(document['window_us']) is not int or document['window_us'] != WINDOW_US:
        raise ValueError('unsupported policy window')
    training = document['training']
    if not isinstance(training, dict) or set(training) != {'source', 'seen_frames', 'normal_frames'}:
        raise ValueError('invalid training provenance')
    if not isinstance(training['source'], dict):
        raise ValueError('missing training source')
    if any(type(training[k]) is not int or not 1 <= training[k] <= (1<<63)-1 for k in ('seen_frames', 'normal_frames')):
        raise ValueError('invalid training frame counts')
    if training['normal_frames'] > training['seen_frames']:
        raise ValueError('normal count exceeds total')
    source = training['source']
    required = {'input', 'member', 'complete', 'has_labels', 'rows', 'input_sha256', 'member_sha256', 'frames_sha256'}
    if not required.issubset(source):
        raise ValueError('missing completed training provenance')
    if source['complete'] is not True or source['has_labels'] is not True:
        raise ValueError('training source must be complete and labelled')
    if type(source['rows']) is not int or source['rows'] != training['seen_frames']:
        raise ValueError('training source row count mismatch')
    if not isinstance(source['input'], str) or not source['input']:
        raise ValueError('missing training source input')
    if source['member'] is not None and (not isinstance(source['member'], str) or not source['member']):
        raise ValueError('invalid training member')
    for key in ('input_sha256', 'member_sha256', 'frames_sha256'):
        if not isinstance(source[key], str) or not re.fullmatch('[0-9a-f]{64}', source[key]):
            raise ValueError('invalid training source digest')
    entries = document['profiles']
    if not isinstance(entries, list) or not entries:
        raise ValueError('no normal profiles')
    seen = set()
    total = 0
    for e in entries:
        if not isinstance(e, dict) or set(e) != {'bus', 'can_id', 'dlcs', 'min_interval_us', 'interval_pairs', 'max_count_1s', 'normal_frames'}:
            raise ValueError('invalid profile fields')
        if e['bus'] not in BUSES or type(e['can_id']) is not int or not 0 <= e['can_id'] <= 0x7ff:
            raise ValueError('invalid profile bus/ID')
        key = (e['bus'], e['can_id'])
        if key in seen:
            raise ValueError('duplicate profile')
        seen.add(key)
        dlcs = e['dlcs']
        if not isinstance(dlcs, list) or not dlcs or any(type(d) is not int or not 0 <= d <= 8 for d in dlcs):
            raise ValueError('invalid allowed DLCs')
        if dlcs != sorted(set(dlcs)):
            raise ValueError('allowed DLCs must be unique and sorted')
        for k in ('normal_frames', 'max_count_1s'):
            if type(e[k]) is not int or not 1 <= e[k] <= (1<<63)-1:
                raise ValueError('invalid profile frame count')
        pairs = e['interval_pairs']
        minimum = e['min_interval_us']
        if type(pairs) is not int or not 0 <= pairs < e['normal_frames']:
            raise ValueError('invalid interval support')
        if (pairs == 0 and minimum is not None) or (pairs > 0 and (type(minimum) is not int or not 0 <= minimum <= (1<<63)-1)):
            raise ValueError('invalid minimum interval')
        if e['max_count_1s'] > e['normal_frames']:
            raise ValueError('window count exceeds normal frames')
        total += e['normal_frames']
    if total != training['normal_frames']:
        raise ValueError('profile counts differ from training count')
    return deepcopy(document)


class ProfileBuilder:
    def __init__(self):
        self._last_time = None
        self._previous = {}
        self._profiles = {}
        self._windows = {}
        self.seen = self.normal = 0

    def observe(self, frame: Frame, is_normal: bool):
        if self._last_time is not None and frame.timestamp_us < self._last_time:
            raise ValueError('timestamp regression during fit')
        self._last_time = frame.timestamp_us
        self.seen += 1
        key = (frame.bus, frame.can_id)
        previous = self._previous.get(key)
        self._previous[key] = (frame.timestamp_us, is_normal)
        if not is_normal:
            return
        self.normal += 1
        if key not in self._profiles:
            self._profiles[key] = {'bus': frame.bus, 'can_id': frame.can_id, 'dlcs': set(),
                                   'min_interval_us': None, 'interval_pairs': 0,
                                   'max_count_1s': 0, 'normal_frames': 0}
            self._windows[key] = deque()
        e = self._profiles[key]
        e['normal_frames'] += 1
        e['dlcs'].add(frame.dlc)
        if previous is not None and previous[1]:
            delta = frame.timestamp_us - previous[0]
            e['interval_pairs'] += 1
            e['min_interval_us'] = delta if e['min_interval_us'] is None else min(delta, e['min_interval_us'])
        w = self._windows[key]
        while w and w[0] <= frame.timestamp_us - WINDOW_US:
            w.popleft()
        w.append(frame.timestamp_us)
        e['max_count_1s'] = max(e['max_count_1s'], len(w))

    def finish(self, training):
        entries = []
        for _, raw in sorted(self._profiles.items()):
            e = dict(raw)
            e['dlcs'] = sorted(e['dlcs'])
            entries.append(e)
        p = seal_policy({'schema_version': 1, 'kind': 'normal_observed_envelope', 'window_us': WINDOW_US,
                         'rule_ids': list(RULE_IDS), 'training': {'source': deepcopy(training),
                                                               'seen_frames': self.seen, 'normal_frames': self.normal},
                         'profiles': entries})
        return validate_policy(p)
