"""Frame-level binary and rule metrics; labels never enter the detector."""
from collections import Counter

from .model import BUSES, RULE_IDS, label_family


def _add(counts, attack, alert):
    counts['tp' if attack and alert else 'fn' if attack else 'fp' if alert else 'tn'] += 1


def _report(counts):
    tp, fp, tn, fn = (counts.get(k, 0) for k in ('tp', 'fp', 'tn', 'fn'))

    def ratio(n, d):
        return n / d if d else None

    return {'tp': tp, 'fp': fp, 'tn': tn, 'fn': fn, 'precision': ratio(tp, tp + fp),
            'recall': ratio(tp, tp + fn), 'f1': ratio(2 * tp, 2 * tp + fp + fn),
            'false_positive_rate': ratio(fp, fp + tn), 'accuracy': ratio(tp + tn, tp + fp + tn + fn)}


class Metrics:
    def __init__(self):
        self._binary = Counter()
        self._by_rule = {rule: Counter() for rule in RULE_IDS}
        self._by_bus = {bus: Counter() for bus in BUSES}
        self._classes = {}
        self._raw = Counter()

    def add(self, label, findings, bus):
        family = label_family(label)
        if bus not in BUSES:
            raise ValueError('unknown metric bus')
        rules = {finding['rule_id'] for finding in findings}
        if rules - set(RULE_IDS):
            raise ValueError('unknown metric rule')
        alert, attack = bool(rules), family != 'Normal'
        _add(self._binary, attack, alert)
        _add(self._by_bus[bus], attack, alert)
        for rule in RULE_IDS:
            _add(self._by_rule[rule], attack, rule in rules)
        c = self._classes.setdefault(family, {'frames': 0, 'alerted_frames': 0})
        c['frames'] += 1
        c['alerted_frames'] += alert
        self._raw[label] += 1

    def report(self):
        classes = {}
        for family, c in self._classes.items():
            classes[family] = {**c, 'alert_rate': c['alerted_frames'] / c['frames']}
        return {'binary': _report(self._binary), 'per_rule': {r: _report(c) for r, c in self._by_rule.items()},
                'per_bus': {b: _report(c) for b, c in self._by_bus.items()},
                'per_class': classes, 'raw_labels': dict(self._raw)}
