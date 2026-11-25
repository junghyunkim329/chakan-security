import json
import hashlib
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
from zipfile import ZipFile


ROOT = Path(__file__).resolve().parents[2]
HEADER = 'Interface,Timestamp,Arbitration_ID,DLC,Data,Label\n'
TRAIN = HEADER + 'C-CAN,0,100,2,01 02,Normal\nC-CAN,0.1,100,2,01 02,Normal\nC-CAN,0.2,100,2,01 02,Normal\n'
TEST = HEADER + 'C-CAN,1,100,2,01 02,Normal\nC-CAN,1.01,777,2,01 02,DoS\nC-CAN,1.05,100,2,01 02,Spoofing\nC-CAN,1.06,100,2,01 02,Normal\n'


def command(*args):
    extra=['--backend','python'] if args and args[0] in ('detect','evaluate') else []
    return [sys.executable, '-m', 'IDS.canids', *map(str, args), *extra]


def run(*args):
    return subprocess.run(command(*args), cwd=ROOT, text=True, capture_output=True, timeout=20)


def fit(tmp_path):
    train = tmp_path / 'train.csv'; train.write_text(TRAIN)
    p = tmp_path / 'policy.json'
    result = run('fit', '--input', train, '--output', p)
    assert result.returncode == 0, result.stderr
    return p, train


def test_real_zip_fit_evaluate_events_and_frame_level_metrics(tmp_path):
    zpath = tmp_path / 'data.zip'
    with ZipFile(zpath, 'w') as z:
        z.writestr('Interface/train.csv', TRAIN)
        z.writestr('Interface/test.csv', TEST)
    p = tmp_path / 'policy.json'
    trained = run('fit', '--input', zpath, '--member', 'Interface/train.csv', '--output', p)
    assert trained.returncode == 0, trained.stderr
    frozen = p.read_bytes()
    out = tmp_path / 'evaluation'
    result = run('evaluate', '--input', zpath, '--member', 'Interface/test.csv', '--policy', p, '--output-dir', out)
    assert result.returncode == 0, result.stderr
    report = json.loads((out / 'report.json').read_text())
    manifest = json.loads((out / 'manifest.json').read_text())
    assert report['frames'] == 4 and report['events'] == 3
    assert {k: report['metrics']['binary'][k] for k in ('tp','fp','tn','fn')} == {'tp':2,'fp':1,'tn':1,'fn':0}
    assert report['metrics']['binary']['f1'] == .8
    assert manifest['status'] == 'complete' and manifest['source']['complete']
    expected_engine = hashlib.sha256((ROOT/'IDS/canids/engine.py').read_bytes()).hexdigest()
    assert manifest['implementation_sha256']['engine.py'] == expected_engine
    assert manifest['git_commit']
    events = [json.loads(x) for x in (out / 'ids-events.jsonl').read_text().splitlines()]
    assert [e['source']['line'] for e in events] == [3,4,5]
    assert len({e['event_id'] for e in events}) == 3
    assert all(e['experiment_id'] == manifest['experiment_id'] for e in events)
    assert all(e['policy_hash'] == report['policy_hash'] and e['action'] == 'log_only' for e in events)
    assert all('label' not in e and 'attack_type' not in e for e in events)
    assert p.read_bytes() == frozen


def test_label_free_detect_matches_evaluation_findings(tmp_path):
    p, _ = fit(tmp_path)
    labelled = tmp_path / 'test.csv'; labelled.write_text(TEST)
    plain = tmp_path / 'plain.csv'
    plain.write_text('\n'.join(line.rsplit(',',1)[0] for line in TEST.splitlines()) + '\n')
    a, b = tmp_path/'eval', tmp_path/'detect'
    assert run('evaluate','--input',labelled,'--policy',p,'--output-dir',a).returncode == 0
    result = run('detect','--input',plain,'--policy',p,'--output-dir',b)
    assert result.returncode == 0, result.stderr
    ra, rb = (json.loads((d/'report.json').read_text()) for d in (a,b))
    assert rb['metrics'] is None and rb['events'] == ra['events'] == 3
    def observations(d):
        return [(e['rule_id'],e['source_timestamp_us'],e['observed'],e['threshold'])
                for e in map(json.loads,(d/'ids-events.jsonl').read_text().splitlines())]
    assert observations(a) == observations(b)


def test_parse_failure_preserves_partial_evidence_and_failed_manifest(tmp_path):
    p, _ = fit(tmp_path)
    bad = tmp_path/'bad.csv'; bad.write_text(TEST + 'C-CAN,broken\n')
    out = tmp_path/'failed'
    result = run('evaluate','--input',bad,'--policy',p,'--output-dir',out)
    assert result.returncode != 0
    m = json.loads((out/'manifest.json').read_text())
    assert m['status'] == 'failed'
    assert (out/'ids-events.jsonl').is_file()
    assert not (out/'report.json').exists()


def test_existing_outputs_are_never_overwritten(tmp_path):
    p, train = fit(tmp_path); before = p.read_bytes()
    assert run('fit','--input',train,'--output',p).returncode != 0
    assert p.read_bytes() == before
    out = tmp_path/'existing'; out.mkdir(); (out/'manifest.json').write_text('preserve')
    assert run('evaluate','--input',train,'--policy',p,'--output-dir',out).returncode != 0
    assert (out/'manifest.json').read_text() == 'preserve'


def test_same_frames_with_relabelled_truth_are_marked_calibration_reuse(tmp_path):
    p, train = fit(tmp_path)
    changed = tmp_path/'changed.csv'; changed.write_text(train.read_text().replace('Normal','Replay'))
    out = tmp_path/'reuse'
    result = run('evaluate','--input',changed,'--policy',p,'--output-dir',out)
    assert result.returncode == 0, result.stderr
    assert json.loads((out/'report.json').read_text())['evaluation_kind'] == 'calibration_reuse'


def test_fit_rejects_missing_or_unknown_truth_and_never_writes_policy(tmp_path):
    fit(tmp_path)
    for i, text in enumerate([TRAIN.replace(',Label','').replace(',Normal',''), TRAIN.replace('Normal','unknown')]):
        source=tmp_path/f'bad{i}.csv'; source.write_text(text)
        dest=tmp_path/f'bad{i}.json'
        assert run('fit','--input',source,'--output',dest).returncode != 0
        assert not dest.exists()


def test_sigterm_leaves_interrupted_run_not_success(tmp_path):
    p, _ = fit(tmp_path)
    source=tmp_path/'long.csv';source.write_text(HEADER + 'C-CAN,1,100,2,01 02,Normal\n'*200000)
    out=tmp_path/'interrupted'
    proc=subprocess.Popen(command('evaluate','--input',source,'--policy',p,'--output-dir',out),cwd=ROOT,
                          stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    try:
        deadline=time.monotonic()+5
        while not (out/'manifest.json').exists() and proc.poll() is None and time.monotonic()<deadline:
            time.sleep(.005)
        assert (out/'manifest.json').exists()
        os.kill(proc.pid,signal.SIGTERM)
        _,err=proc.communicate(timeout=10)
        assert proc.returncode==130,err
        assert json.loads((out/'manifest.json').read_text())['status']=='interrupted'
        assert not (out/'report.json').exists()
    finally:
        if proc.poll() is None:
            proc.kill();proc.communicate()


def test_changed_policy_cannot_leave_a_success_report(tmp_path):
    p, _ = fit(tmp_path)
    source = tmp_path/'long.csv'
    source.write_text(HEADER + ''.join(f'C-CAN,{i},100,2,01 02,Normal\n' for i in range(20000)))
    out = tmp_path/'changed-policy'
    proc = subprocess.Popen(command('evaluate','--input',source,'--policy',p,'--output-dir',out),cwd=ROOT,
                            stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    try:
        deadline=time.monotonic()+5
        while not (out/'manifest.json').exists() and proc.poll() is None and time.monotonic()<deadline:
            time.sleep(.002)
        assert (out/'manifest.json').exists()
        p.write_text(p.read_text()+'\n')
        _,err=proc.communicate(timeout=10)
        assert proc.returncode!=0,err
        assert json.loads((out/'manifest.json').read_text())['status']=='failed'
        assert not (out/'report.json').exists()
    finally:
        if proc.poll() is None:
            proc.kill();proc.communicate()
