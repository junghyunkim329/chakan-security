"""Preparation and external evaluation are separate from the native runtime."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from zipfile import ZipFile

import pytest

from test_native_cli import native_binary

ROOT = Path(__file__).resolve().parents[2]
HEADER = 'Interface,Timestamp,Arbitration_ID,DLC,Data,Label\n'
TRAIN = HEADER + 'C-CAN,0,100,2,01 02,Normal\nC-CAN,0.1,100,2,01 02,Normal\nC-CAN,0.2,100,2,01 02,Normal\n'
TEST = HEADER + 'C-CAN,1,100,2,01 02,Normal\nC-CAN,1.01,777,2,01 02,DoS\nC-CAN,1.05,100,2,01 02,Spoofing\nC-CAN,1.06,100,2,01 02,Normal\n'


def run(*args):
    return subprocess.run([sys.executable,'-m','IDS.canids',*map(str,args)],cwd=ROOT,
                          text=True,capture_output=True,timeout=20)


def fit(tmp_path):
    train=tmp_path/'train.csv'; train.write_text(TRAIN)
    policy=tmp_path/'policy.json'
    r=run('fit','--input',train,'--output',policy)
    assert r.returncode==0,r.stderr
    return policy,train


def detect(binary,policy,source,out,*extra):
    r=subprocess.run([str(binary),'--input',str(source),'--policy',str(policy),'--output-dir',str(out),*extra],
                     capture_output=True,text=True,timeout=20)
    assert r.returncode==0,r.stderr


def evaluate(policy,source,native,out,*extra):
    return run('evaluate','--run-dir',native,'--input',source,'--policy',policy,'--output-dir',out,*extra)


def test_real_zip_native_run_then_external_metrics(native_binary,tmp_path):
    z=tmp_path/'data.zip'
    with ZipFile(z,'w') as archive:
        archive.writestr('train.csv',TRAIN);archive.writestr('test.csv',TEST)
    p=tmp_path/'policy.json'
    assert run('fit','--input',z,'--member','train.csv','--output',p).returncode==0
    original=p.read_bytes();native=tmp_path/'native';out=tmp_path/'analysis'
    detect(native_binary,p,z,native,'--member','test.csv')
    event_digest=hashlib.sha256((native/'ids-events.jsonl').read_bytes()).hexdigest()
    r=evaluate(p,z,native,out,'--member','test.csv')
    assert r.returncode==0,r.stderr
    report=json.loads((out/'report.json').read_text())
    assert report['frames']==4 and report['events']==3
    assert {k:report['metrics']['binary'][k] for k in ('tp','fp','tn','fn')}=={'tp':2,'fp':1,'tn':1,'fn':0}
    assert report['metrics']['binary']['f1']==.8
    manifest=json.loads((out/'manifest.json').read_text())
    assert manifest['status']=='complete' and manifest['detection_executed'] is False
    assert not (out/'ids-events.jsonl').exists()
    assert event_digest==hashlib.sha256((native/'ids-events.jsonl').read_bytes()).hexdigest()
    assert original==p.read_bytes()


def prepare(binary,tmp_path):
    p,_=fit(tmp_path);source=tmp_path/'test.csv';source.write_text(TEST)
    native=tmp_path/'native';detect(binary,p,source,native)
    return p,source,native


@pytest.mark.parametrize('change',['partial','digest','position','duplicate','source-path'])
def test_external_evaluation_rejects_invalid_evidence(native_binary,tmp_path,change):
    p,source,native=prepare(native_binary,tmp_path)
    manifest=json.loads((native/'manifest.json').read_text())
    if change=='partial': manifest['status']='interrupted'
    else:
        file=native/'ids-events.jsonl'
        events=[json.loads(x) for x in file.read_text().splitlines()]
        if change=='position': events[0]['can_id']='0x100'
        elif change=='source-path': events[0]['source']['input']='another.csv'
        elif change=='duplicate': events[1]=dict(events[0]);events[1]['event_id']=f"{manifest['experiment_id']}:2"
        else: events[0]['observed']='tampered'
        file.write_text(''.join(json.dumps(e)+'\n' for e in events))
        if change!='digest': manifest['outputs_sha256']['ids-events.jsonl']=hashlib.sha256(file.read_bytes()).hexdigest()
    (native/'manifest.json').write_text(json.dumps(manifest))
    out=tmp_path/'analysis';r=evaluate(p,source,native,out)
    assert r.returncode!=0
    assert json.loads((out/'manifest.json').read_text())['status']=='failed'
    assert not (out/'report.json').exists()


def test_external_evaluation_needs_matching_source_and_frozen_policy(native_binary,tmp_path):
    p,source,native=prepare(native_binary,tmp_path)
    source.write_text(TEST.replace('1.01','1.02'))
    r=evaluate(p,source,native,tmp_path/'wrong-source')
    assert r.returncode!=0
    source.write_text(TEST);p.write_text(p.read_text()+'\n')
    r=evaluate(p,source,native,tmp_path/'wrong-policy')
    assert r.returncode!=0


def test_existing_outputs_are_not_overwritten(native_binary,tmp_path):
    p,train=fit(tmp_path); before=p.read_bytes()
    assert run('fit','--input',train,'--output',p).returncode!=0 and p.read_bytes()==before
    native=tmp_path/'native';detect(native_binary,p,train,native)
    out=tmp_path/'existing';out.mkdir();(out/'manifest.json').write_text('preserve')
    assert evaluate(p,train,native,out).returncode!=0
    assert (out/'manifest.json').read_text()=='preserve'


def test_same_frames_with_relabelled_truth_are_calibration_reuse(native_binary,tmp_path):
    p,train=fit(tmp_path);source=tmp_path/'changed.csv';source.write_text(train.read_text().replace('Normal','Replay'))
    native=tmp_path/'native';out=tmp_path/'analysis';detect(native_binary,p,source,native)
    r=evaluate(p,source,native,out)
    assert r.returncode==0,r.stderr
    assert json.loads((out/'report.json').read_text())['evaluation_kind']=='calibration_reuse'


def test_python_has_no_live_detection_command():
    assert run('detect','--help').returncode!=0


def test_fit_rejects_missing_or_unknown_truth(tmp_path):
    for i,text in enumerate([TRAIN.replace(',Label','').replace(',Normal',''),TRAIN.replace('Normal','unknown')]):
        source=tmp_path/f'bad{i}.csv';source.write_text(text);p=tmp_path/f'bad{i}.json'
        assert run('fit','--input',source,'--output',p).returncode!=0
        assert not p.exists()
