"""Python reference oracle for tests; production detection runs in native C."""
from collections import deque

from .model import Frame
from .profile import validate_policy


class Detector:
    def __init__(self, policy):
        p = validate_policy(policy)
        self.policy_hash = p['policy_hash']
        self._profiles = {(e['bus'], e['can_id']): e for e in p['profiles']}
        self._window_us = p['window_us']
        self._previous = {}
        self._windows = {}
        self._last_time = None

    def observe(self, frame: Frame):
        if not isinstance(frame, Frame):
            raise TypeError('Detector accepts only truth-free Frame objects')
        ts = frame.timestamp_us
        if self._last_time is not None and ts < self._last_time:
            raise ValueError('timestamp regression during detection')
        self._last_time = ts
        key = (frame.bus, frame.can_id)
        e = self._profiles.get(key)
        hits = []

        def hit(rule, observed, threshold, window=None):
            hits.append({'rule_id': rule, 'bus': frame.bus, 'can_id': f'0x{frame.can_id:03X}',
                         'source_timestamp_us': ts, 'observed': observed, 'threshold': threshold,
                         'window_us': window})

        if e is None:
            hit('CAN_ID_UNSEEN', f'0x{frame.can_id:03X}', 'ID absent from this bus normal profile')
            return hits
        if frame.dlc not in e['dlcs']:
            hit('CAN_DLC_UNSEEN', frame.dlc, list(e['dlcs']))
        previous = self._previous.get(key)
        minimum = e['min_interval_us']
        if previous is not None and minimum is not None and ts - previous < minimum:
            hit('CAN_INTERVAL_SHORT', ts - previous, minimum)
        self._previous[key] = ts
        w = self._windows.setdefault(key, deque())
        while w and w[0] <= ts - self._window_us:
            w.popleft()
        w.append(ts)
        if len(w) > e['max_count_1s']:
            hit('CAN_RATE_HIGH', len(w), e['max_count_1s'], self._window_us)
        return hits
