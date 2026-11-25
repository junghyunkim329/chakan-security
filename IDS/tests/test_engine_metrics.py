import pytest

from IDS.canids.model import Frame
from IDS.canids.profile import seal_policy
from IDS.canids.engine import Detector
from IDS.canids.evaluate import Metrics


def policy():
    return seal_policy({'schema_version': 1, 'kind': 'normal_observed_envelope', 'window_us': 1000000,
                        'rule_ids': ['CAN_ID_UNSEEN', 'CAN_DLC_UNSEEN', 'CAN_INTERVAL_SHORT', 'CAN_RATE_HIGH'],
                        'training': {'source': {'input': 'synthetic.csv', 'member': None, 'complete': True,
                                                'has_labels': True, 'rows': 3, 'input_sha256': '0' * 64,
                                                'member_sha256': '0' * 64, 'frames_sha256': '0' * 64},
                                     'seen_frames': 3, 'normal_frames': 3},
                        'profiles': [{'bus': 'C-CAN', 'can_id': 0x100, 'dlcs': [2], 'min_interval_us': 100,
                                      'interval_pairs': 2, 'max_count_1s': 2, 'normal_frames': 3}]})


def f(t, bus='C-CAN', can_id=0x100, data=b'\x00\x00'):
    return Frame(bus, t, can_id, len(data), data)


def rules(findings):
    return {x['rule_id'] for x in findings}


def test_first_frame_and_equal_minimum_interval_are_normal():
    d = Detector(policy())
    assert d.observe(f(0)) == []
    assert d.observe(f(100)) == []


def test_strict_short_interval_and_rate_excess_include_observation_evidence():
    d = Detector(policy())
    d.observe(f(0)); d.observe(f(100))
    hits = d.observe(f(150))
    assert rules(hits) == {'CAN_INTERVAL_SHORT', 'CAN_RATE_HIGH'}
    by_rule = {x['rule_id']: x for x in hits}
    assert by_rule['CAN_INTERVAL_SHORT']['observed'] == 50
    assert by_rule['CAN_INTERVAL_SHORT']['threshold'] == 100
    assert by_rule['CAN_RATE_HIGH']['observed'] == 3
    assert by_rule['CAN_RATE_HIGH']['threshold'] == 2
    assert by_rule['CAN_RATE_HIGH']['window_us'] == 1000000


def test_window_excludes_exactly_one_second_old_frame():
    d = Detector(policy())
    assert not d.observe(f(0))
    assert not d.observe(f(999999))
    assert rules(d.observe(f(1000000))) == {'CAN_INTERVAL_SHORT'}


def test_unknown_ids_have_no_invented_timing_baseline_and_do_not_auto_learn():
    d = Detector(policy())
    for t in (0, 1, 2):
        assert rules(d.observe(f(t, can_id=0x777))) == {'CAN_ID_UNSEEN'}


def test_same_id_on_another_bus_does_not_reuse_profile_or_timing():
    d = Detector(policy())
    assert not d.observe(f(1000))
    assert rules(d.observe(f(1050, bus='B-CAN'))) == {'CAN_ID_UNSEEN'}
    assert not d.observe(f(1100))


def test_anomalous_dlc_frame_still_updates_observed_timing_and_rate():
    d = Detector(policy())
    d.observe(f(0))
    assert rules(d.observe(f(500, data=b'\x00'))) == {'CAN_DLC_UNSEEN'}
    assert rules(d.observe(f(550))) == {'CAN_INTERVAL_SHORT', 'CAN_RATE_HIGH'}


def test_timestamp_regression_is_visible_and_equal_times_are_permitted():
    d = Detector(policy())
    d.observe(f(10))
    assert rules(d.observe(f(10))) == {'CAN_INTERVAL_SHORT'}
    with pytest.raises(ValueError, match='timestamp'):
        d.observe(f(9))


def test_policy_is_snapshotted_and_never_changed_by_detection():
    p = policy(); d = Detector(p)
    p['profiles'][0]['max_count_1s'] = 1000
    d.observe(f(0)); d.observe(f(100))
    assert 'CAN_RATE_HIGH' in rules(d.observe(f(200)))


def test_no_interval_support_skips_only_interval_rule():
    p = policy(); e = p['profiles'][0]
    e['interval_pairs'] = 0; e['min_interval_us'] = None
    d = Detector(seal_policy(p))
    assert not d.observe(f(0))
    assert not d.observe(f(0))
    assert rules(d.observe(f(0))) == {'CAN_RATE_HIGH'}


def test_payload_contents_do_not_become_a_known_attack_signature():
    a, b = Detector(policy()), Detector(policy())
    assert a.observe(f(0, data=b'\xff\xff')) == b.observe(f(0, data=b'\x00\x00'))


def test_union_metrics_count_frame_once_and_preserve_raw_uds_family():
    m = Metrics()
    m.add('DoS', [{'rule_id': 'CAN_ID_UNSEEN'}, {'rule_id': 'CAN_RATE_HIGH'}], 'C-CAN')
    m.add('Normal', [{'rule_id': 'CAN_DLC_UNSEEN'}], 'C-CAN')
    m.add('Normal', [], 'B-CAN')
    m.add('Spoofing_UDS_P_7E0_B4', [], 'P-CAN')
    r = m.report()
    assert {k: r['binary'][k] for k in ['tp', 'fp', 'tn', 'fn']} == {'tp': 1, 'fp': 1, 'tn': 1, 'fn': 1}
    assert r['binary']['precision'] == .5 and r['binary']['recall'] == .5
    assert r['binary']['f1'] == .5 and r['binary']['false_positive_rate'] == .5
    assert r['per_rule']['CAN_ID_UNSEEN']['tp'] == 1
    assert r['per_rule']['CAN_ID_UNSEEN']['fp'] == 0
    assert r['per_class']['UDS_Spoofing']['frames'] == 1
    assert r['raw_labels']['Spoofing_UDS_P_7E0_B4'] == 1
    assert r['per_bus']['B-CAN']['tn'] == 1


def test_empty_metric_denominators_are_null_and_unknown_truth_is_rejected():
    m = Metrics()
    assert m.report()['binary']['recall'] is None
    with pytest.raises(ValueError):
        m.add('invented', [], 'C-CAN')
