"""Native process contracts; no Python executable exists in its PATH."""
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import time
from zipfile import ZipFile, ZIP_STORED

import pytest

from IDS.canids.model import Frame
from IDS.canids.profile import ProfileBuilder, seal_policy

ROOT = Path(__file__).resolve().parents[2]
HEADER = 'Interface,Timestamp,Arbitration_ID,DLC,Data,Label\n'
ROWS = ('C-CAN,1,100,2,01 02,Normal\n'
        'C-CAN,1.01,777,2,01 02,DoS\n'
        'C-CAN,1.05,100,2,01 02,Spoofing\n'
        'C-CAN,1.06,100,2,01 02,Normal\n')


@pytest.fixture(scope='session')
def native_binary(tmp_path_factory):
    build = tmp_path_factory.mktemp('native-ids-build')
    for cmd in (['cmake', '-S', str(ROOT/'IDS'), '-B', str(build), '-DCMAKE_BUILD_TYPE=Release'],
                ['cmake', '--build', str(build)]):
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        assert r.returncode == 0, r.stdout + r.stderr
    return build/'canids'


@pytest.fixture
def native_policy(tmp_path):
    b = ProfileBuilder()
    for ts in (0, 100000, 200000):
        b.observe(Frame('C-CAN', ts, 0x100, 2, b'\1\2'), True)
    source = {'input': 'synthetic.csv', 'member': None, 'complete': True, 'has_labels': True,
              'rows': 3, 'input_sha256': '0'*64, 'member_sha256': '0'*64, 'frames_sha256': '0'*64}
    p = tmp_path/'policy.json'
    p.write_text(json.dumps(b.finish(source)))
    return p


def native_run(binary, policy, source, out, *extra):
    assert binary.is_file(), 'standalone C IDS executable is missing'
    return subprocess.run([str(binary), '--input', str(source), '--policy', str(policy),
                           '--output-dir', str(out), *extra], capture_output=True, text=True,
                          env={**os.environ, 'PATH': ''}, timeout=20)


def load_events(out):
    return [json.loads(line) for line in (out/'ids-events.jsonl').read_text().splitlines()]


def test_native_zip_without_python_logs_literal_findings(native_binary, native_policy, tmp_path):
    source = tmp_path/'data.zip'
    with ZipFile(source, 'w') as z:
        z.writestr('Interface/test.csv', HEADER + ROWS)
    out = tmp_path/'run'
    r = native_run(native_binary, native_policy, source, out, '--member', 'Interface/test.csv')
    assert r.returncode == 0, r.stderr
    events = load_events(out)
    assert [(e['rule_id'], e['source']['line']) for e in events] == [
        ('CAN_ID_UNSEEN', 3), ('CAN_INTERVAL_SHORT', 4), ('CAN_INTERVAL_SHORT', 5)]
    assert [(e['observed'], e['threshold']) for e in events[1:]] == [(50000, 100000), (10000, 100000)]
    m = json.loads((out/'manifest.json').read_text())
    assert m['status'] == 'complete' and m['source']['rows'] == 4
    assert m['source']['member_sha256'] == hashlib.sha256((HEADER+ROWS).encode()).hexdigest()
    assert m['backend']['backend'] == 'c11_standalone'
    assert m['backend']['executable_sha256'] == hashlib.sha256(native_binary.read_bytes()).hexdigest()
    assert not m['can_transmission_executed'] and not m['prevention_enabled']
    assert all('label' not in e and e['action'] == 'log_only' for e in events)
    runtime = json.loads((out/'runtime.json').read_text())
    assert runtime['frames'] == 4 and runtime['events'] == 3
    assert runtime['logging']['events_written'] == 3 and runtime['logging']['events_dropped'] == 0
    assert runtime['logging']['latency_samples'] == 3


def test_csv_quotes_crlf_and_labels_do_not_change_detection(native_binary, native_policy, tmp_path):
    a = tmp_path/'a.csv'; b = tmp_path/'b.csv'
    a.write_text(HEADER+ROWS)
    b.write_bytes((HEADER+ROWS).replace('C-CAN', '"C-CAN"').replace('01 02', '"01 02"')
                  .replace('Normal', 'Anything').replace('\n', '\r\n').encode())
    aa, bb = tmp_path/'aa', tmp_path/'bb'
    for source, out in ((a,aa),(b,bb)):
        r = native_run(native_binary, native_policy, source, out)
        assert r.returncode == 0, r.stderr
    fields = ('rule_id','bus','can_id','observed','threshold','source_timestamp_us','window_us')
    assert [[e[k] for k in fields] for e in load_events(aa)] == [[e[k] for k in fields] for e in load_events(bb)]
    ma, mb = [json.loads((out/'manifest.json').read_text()) for out in (aa,bb)]
    assert ma['source']['frames_sha256'] == mb['source']['frames_sha256']


def test_all_rules_and_visible_window_growth(native_binary,native_policy,tmp_path):
    source=tmp_path/'source.csv'
    source.write_text(HEADER+ROWS+'C-CAN,1.07,100,1,01,Fuzzing\n')
    out=tmp_path/'run'
    r=native_run(native_binary,native_policy,source,out)
    assert r.returncode==0,r.stderr
    assert [e['rule_id'] for e in load_events(out)][-3:]==[
        'CAN_DLC_UNSEEN','CAN_INTERVAL_SHORT','CAN_RATE_HIGH']
    d=json.loads((out/'runtime.json').read_text())['detector']
    assert d['initial_capacity_slots']==3 and d['capacity_slots']>=4
    assert d['window_grows']==1 and d['largest_active_id_window']==4


@pytest.mark.parametrize('bad', ['C-CAN,broken\n', 'C-CAN,0,100,2,01 02,Normal\n',
    'C-CAN,2,100,8,01 02,Normal\n', 'C-CAN,2,100,2,"01 02,Normal\n',
    'C-CAN,9223372036854.775808,100,2,01 02,Normal\n'])
def test_native_invalid_input_retains_failed_evidence(native_binary,native_policy,tmp_path,bad):
    source=tmp_path/'bad.csv'; source.write_text(HEADER+ROWS+bad)
    out=tmp_path/'failed'
    r=native_run(native_binary,native_policy,source,out)
    assert r.returncode != 0
    assert json.loads((out/'manifest.json').read_text())['status'] == 'failed'
    assert not (out/'runtime.json').exists()


@pytest.mark.parametrize('mutation', ['hash','provenance','counts','duplicate','float','missing-null'])
def test_native_policy_rejects_invalid_frozen_contract(native_binary,native_policy,tmp_path,mutation):
    p=json.loads(native_policy.read_text())
    if mutation=='hash': p['profiles'][0]['max_count_1s']=1
    else:
        if mutation=='provenance': del p['training']['source']['frames_sha256']
        if mutation=='counts': p['profiles'][0]['normal_frames']=4
        if mutation=='duplicate': p['profiles'].append(p['profiles'][0])
        if mutation=='float': p['profiles'][0]['can_id']=256.0
        if mutation=='missing-null':
            p['profiles'][0]['interval_pairs']=0
            del p['profiles'][0]['min_interval_us']
            p['profiles'][0]['unexpected']=None
        p=seal_policy(p)
    native_policy.write_text(json.dumps(p))
    source=tmp_path/'source.csv'; source.write_text(HEADER+ROWS)
    r=native_run(native_binary,native_policy,source,tmp_path/'bad')
    assert r.returncode != 0


def test_native_policy_unicode_canonical_hash_and_output_exclusion(native_binary,native_policy,tmp_path):
    p=json.loads(native_policy.read_text()); p['training']['source']['input']='실험/🚗.csv'
    native_policy.write_text(json.dumps(seal_policy(p),ensure_ascii=False))
    source=tmp_path/'source.csv'; source.write_text(HEADER+ROWS)
    out=tmp_path/'run'
    assert native_run(native_binary,native_policy,source,out).returncode == 0
    before=(out/'manifest.json').read_bytes()
    assert native_run(native_binary,native_policy,source,out).returncode != 0
    assert (out/'manifest.json').read_bytes() == before


@pytest.mark.parametrize('value',[float('nan'),float('inf'),-float('inf')])
def test_nonfinite_metadata_is_not_a_valid_policy(native_binary,native_policy,tmp_path,value):
    p=json.loads(native_policy.read_text());p.pop('policy_hash')
    p['training']['source']['producer']={'elapsed_s':value}
    p['policy_hash']=hashlib.sha256(json.dumps(p,sort_keys=True,separators=(',',':')).encode()).hexdigest()
    native_policy.write_text(json.dumps(p))
    source=tmp_path/'source.csv';source.write_text(HEADER+ROWS)
    assert native_run(native_binary,native_policy,source,tmp_path/'run').returncode!=0


@pytest.mark.parametrize('ascii_only',[True,False])
def test_valid_python_policy_with_del_in_provenance(native_binary,native_policy,tmp_path,ascii_only):
    p=json.loads(native_policy.read_text());p['training']['source']['input']='synthetic'+chr(127)+'.csv'
    native_policy.write_text(json.dumps(seal_policy(p),ensure_ascii=ascii_only))
    source=tmp_path/'source.csv';source.write_text(HEADER+ROWS)
    r=native_run(native_binary,native_policy,source,tmp_path/'run')
    assert r.returncode==0,r.stderr


def test_bad_zip_crc_is_not_success(native_binary,native_policy,tmp_path):
    source=tmp_path/'bad.zip'
    with ZipFile(source,'w',compression=ZIP_STORED) as z:
        z.writestr('test.csv',HEADER+ROWS)
    data=source.read_bytes(); at=data.index(b'01 02'); source.write_bytes(data[:at]+b'03 02'+data[at+5:])
    out=tmp_path/'run'
    r=native_run(native_binary,native_policy,source,out,'--member','test.csv')
    assert r.returncode != 0
    assert json.loads((out/'manifest.json').read_text())['status'] == 'failed'


def test_oversized_event_is_written_without_truncation(native_binary,native_policy,tmp_path):
    name='Interface/'+'x'*10000+'.csv'
    source=tmp_path/'long-member.zip'
    with ZipFile(source,'w') as z:
        z.writestr(name,HEADER+ROWS)
    out=tmp_path/'run'
    r=native_run(native_binary,native_policy,source,out,'--member',name)
    assert r.returncode==0,r.stderr
    assert all(e['source']['member']==name for e in load_events(out))
    logging=json.loads((out/'runtime.json').read_text())['logging']
    assert logging['oversized_records']==3 and logging['events_written']==3


def test_native_sigterm_is_interrupted(native_binary,native_policy,tmp_path):
    source=tmp_path/'long.csv'; source.write_text(HEADER+'C-CAN,1,100,2,01 02,Normal\n'*500000)
    out=tmp_path/'run'
    p=subprocess.Popen([str(native_binary),'--input',str(source),'--policy',str(native_policy),
                        '--output-dir',str(out)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    try:
        deadline=time.monotonic()+10
        while not (out/'manifest.json').exists() and p.poll() is None and time.monotonic()<deadline:
            time.sleep(.001)
        assert (out/'manifest.json').exists()
        p.send_signal(signal.SIGTERM)
        _,err=p.communicate(timeout=10)
        assert p.returncode==130,err
        assert json.loads((out/'manifest.json').read_text())['status']=='interrupted'
        assert not (out/'runtime.json').exists()
    finally:
        if p.poll() is None: p.kill(); p.communicate()
