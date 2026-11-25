import copy
import hashlib
import json
from zipfile import ZIP_STORED, ZipFile

import pytest

from IDS.canids.model import Frame, label_family
from IDS.canids.source import Source
from IDS.canids.profile import ProfileBuilder, seal_policy, validate_policy


HEADER = 'Interface,Timestamp,Arbitration_ID,DLC,Data,Label\n'


def write_csv(tmp_path, rows, header=HEADER):
    path = tmp_path / 'frames.csv'
    path.write_text(header + rows)
    return path


def frame(ts, can_id=0x100, bus='C-CAN', data=b'\x01\x02'):
    return Frame(bus, ts, can_id, len(data), data)


def provenance(rows):
    return {'input': 'synthetic.csv', 'member': None, 'complete': True, 'has_labels': True,
            'rows': rows, 'input_sha256': '0' * 64, 'member_sha256': '0' * 64,
            'frames_sha256': '0' * 64}


def test_hex_id_short_payload_and_zero_dlc_preserved(tmp_path):
    p = write_csv(tmp_path, 'C-CAN,0.00059,329,2,01 ff,Normal\nB-CAN,0.00059,700,0,,Replay\n')
    with Source(p) as source:
        rows = list(source)
    assert rows[0].frame == Frame('C-CAN', 590, 0x329, 2, b'\x01\xff')
    assert rows[0].line == 2 and rows[0].label == 'Normal'
    assert rows[1].frame.data == b''
    assert source.metadata['rows'] == 2
    assert source.metadata['complete']
    assert source.metadata['member_sha256'] == hashlib.sha256(p.read_bytes()).hexdigest()
    assert not hasattr(rows[0].frame, 'label')


@pytest.mark.parametrize('row', [
    'C-CAN,nan,100,2,01 02,Normal',
    'C-CAN,-1,100,2,01 02,Normal',
    'C-CAN,0.0000001,100,2,01 02,Normal',
    'C-CAN,9223372036854.775808,100,2,01 02,Normal',
    'C-CAN,0,800,2,01 02,Normal',
    'C-CAN,0,100,9,01 02,Normal',
    'C-CAN,0,100,2,01,Normal',
    'C-CAN,0,100,2,GG 02,Normal',
    'C-CAN,0,100,2,1 02,Normal',
    'UNKNOWN,0,100,2,01 02,Normal',
    'C-CAN,0,100,2,01 02',
])
def test_invalid_input_is_error_not_normal_or_dropped(tmp_path, row):
    p = write_csv(tmp_path, row + '\n')
    with Source(p) as source, pytest.raises(ValueError):
        list(source)
    assert not source.metadata['complete']


def test_label_free_data_supported_and_schema_is_not_guessed(tmp_path):
    p = write_csv(tmp_path, 'C-CAN,1,100,0,\n', HEADER.removesuffix(',Label\n') + '\n')
    with Source(p) as s:
        assert list(s)[0].label is None
    p.write_text('Timestamp,Arbitration_ID,DLC,Data\n0,100,0,\n')
    with pytest.raises(ValueError):
        with Source(p) as s:
            list(s)


def test_unterminated_quoted_final_field_cannot_be_successful_source(tmp_path):
    p = write_csv(tmp_path, 'C-CAN,0,100,1,00,"Normal')
    with Source(p) as source, pytest.raises(ValueError):
        list(source)
    assert not source.metadata['complete']


def test_zip_member_is_read_without_extracting_and_crc_failure_surfaces(tmp_path):
    p = tmp_path / 'source.zip'
    content = (HEADER + 'C-CAN,0,100,2,01 02,Normal\n').encode()
    with ZipFile(p, 'w', compression=ZIP_STORED) as z:
        z.writestr('nested/data.csv', content)
    with Source(p, 'nested/data.csv') as s:
        assert len(list(s)) == 1
    assert not (tmp_path / 'nested').exists()
    raw = p.read_bytes()
    p.write_bytes(raw.replace(b'01 02', b'03 02', 1))
    with pytest.raises(Exception, match='CRC'):
        with Source(p, 'nested/data.csv') as s:
            list(s)


def test_unknown_labels_fail_and_uds_family_preserves_input_semantics():
    assert label_family('Spoofing_UDS_P_7E0_B4') == 'UDS_Spoofing'
    assert label_family('Normal') == 'Normal'
    with pytest.raises(ValueError):
        label_family('unrecognised')


def test_frame_fingerprint_ignores_truth_label_but_raw_digest_does_not(tmp_path):
    p = write_csv(tmp_path, 'C-CAN,0,100,2,01 02,Normal\n')
    with Source(p) as a:
        list(a)
    p.write_text(HEADER + 'C-CAN,0,100,2,01 02,Spoofing\n')
    with Source(p) as b:
        list(b)
    assert a.metadata['frames_sha256'] == b.metadata['frames_sha256']
    assert a.metadata['member_sha256'] != b.metadata['member_sha256']


def test_only_normal_frames_fit_and_attack_interrupted_pairs_excluded():
    b = ProfileBuilder()
    for f, normal in [(frame(0), True), (frame(100), True), (frame(150), False),
                      (frame(160), True), (frame(170, 0x777), False), (frame(300), True)]:
        b.observe(f, normal)
    p = b.finish(provenance(6))
    assert len(p['profiles']) == 1
    entry = p['profiles'][0]
    assert entry['min_interval_us'] == 100
    assert entry['interval_pairs'] == 2
    assert entry['max_count_1s'] == 4
    assert entry['normal_frames'] == 4
    assert entry['dlcs'] == [2]
    assert p['training']['seen_frames'] == 6
    assert p['training']['normal_frames'] == 4


def test_bus_isolation_and_window_open_left_edge_in_fit():
    b = ProfileBuilder()
    for f in [frame(0), frame(10, bus='B-CAN'), frame(999999), frame(1000000)]:
        b.observe(f, True)
    p = b.finish(provenance(4))
    entries = {e['bus']: e for e in p['profiles']}
    assert entries['C-CAN']['max_count_1s'] == 2
    assert entries['C-CAN']['min_interval_us'] == 1
    assert entries['B-CAN']['min_interval_us'] is None


def test_policy_hash_tampering_and_invalid_resigned_window_rejected():
    b = ProfileBuilder(); b.observe(frame(0), True)
    p = b.finish(provenance(1))
    assert validate_policy(json.loads(json.dumps(p)))['policy_hash'] == p['policy_hash']
    changed = copy.deepcopy(p); changed['profiles'][0]['max_count_1s'] = 99
    with pytest.raises(ValueError, match='hash'):
        validate_policy(changed)
    changed = copy.deepcopy(p); changed['window_us'] = -1
    with pytest.raises(ValueError):
        validate_policy(seal_policy(changed))


def test_fit_rejects_empty_normal_and_regression_but_accepts_equal_time():
    b = ProfileBuilder(); b.observe(frame(10), False)
    with pytest.raises(ValueError):
        b.finish(provenance(1))
    b.observe(frame(10), True)
    with pytest.raises(ValueError, match='timestamp'):
        b.observe(frame(9), True)


@pytest.mark.parametrize('change', ['missing', 'bad_digest', 'incomplete', 'wrong_rows', 'unlabelled'])
def test_resigned_policy_cannot_omit_or_corrupt_training_provenance(change):
    b = ProfileBuilder(); b.observe(frame(0), True)
    p = b.finish(provenance(1))
    source = p['training']['source']
    if change == 'missing':
        p['training']['source'] = {}
    elif change == 'bad_digest':
        source['frames_sha256'] = 'not-a-digest'
    elif change == 'incomplete':
        source['complete'] = False
    elif change == 'wrong_rows':
        source['rows'] = 2
    else:
        source['has_labels'] = False
    with pytest.raises(ValueError):
        validate_policy(seal_policy(p))
