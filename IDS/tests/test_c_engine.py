from pathlib import Path
import random
import shutil
import subprocess

import pytest

from IDS.canids.c_engine import CDetector
from IDS.canids.engine import Detector
from IDS.canids.model import Frame
from IDS.canids.profile import seal_policy

ROOT=Path(__file__).resolve().parents[1]


@pytest.fixture(scope='session')
def c_library(tmp_path_factory):
    build=tmp_path_factory.mktemp('canids-c-build')
    for cmd in [['cmake','-S',str(ROOT),'-B',str(build),'-DCMAKE_BUILD_TYPE=Release'],
                ['cmake','--build',str(build)],['ctest','--test-dir',str(build),'--output-on-failure']]:
        result=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
        assert result.returncode==0,result.stdout+result.stderr
    return build/'libcanids.so'


def policy():
    return seal_policy({'schema_version':1,'kind':'normal_observed_envelope','window_us':1000000,
        'rule_ids':['CAN_ID_UNSEEN','CAN_DLC_UNSEEN','CAN_INTERVAL_SHORT','CAN_RATE_HIGH'],
        'training':{'source':{'input':'synthetic.csv','member':None,'complete':True,'has_labels':True,
            'rows':3,'input_sha256':'0'*64,'member_sha256':'0'*64,'frames_sha256':'0'*64},
            'seen_frames':3,'normal_frames':3},
        'profiles':[{'bus':'C-CAN','can_id':0x100,'dlcs':[2],'min_interval_us':100,
            'interval_pairs':2,'max_count_1s':2,'normal_frames':3}]})


def test_c_core_equality_bounds_and_observation_contract(c_library):
    with CDetector(policy(),c_library) as d:
        assert d.observe(Frame('C-CAN',0,0x100,2,b'00'))==[]
        assert d.observe(Frame('C-CAN',100,0x100,2,b'00'))==[]
        hits=d.observe(Frame('C-CAN',150,0x100,2,b'00'))
        assert [h['rule_id'] for h in hits]==['CAN_INTERVAL_SHORT','CAN_RATE_HIGH']
        assert [(h['observed'],h['threshold']) for h in hits]==[(50,100),(3,2)]
        assert d.backend_info['backend']=='c11'
        assert len(d.backend_info['library_sha256'])==64
    with pytest.raises(ValueError):
        d.observe(Frame('C-CAN',200,0x100,2,b'00'))


def test_c_core_matches_python_for_known_unknown_dlc_and_window_sequences(c_library):
    rng=random.Random(20261124)
    a=Detector(policy())
    ts=0
    with CDetector(policy(),c_library) as b:
        for _ in range(10000):
            ts+=rng.choice([0,50,100,50000,1000000])
            data=bytes([rng.randrange(256)])*rng.choice([0,1,2,8])
            f=Frame(rng.choice(['C-CAN','P-CAN','B-CAN']),ts,rng.choice([0x100,0x777]),len(data),data)
            assert a.observe(f)==b.observe(f)


def test_c_time_regression_is_error_and_future_processing_remains_valid(c_library):
    with CDetector(policy(),c_library) as d:
        assert not d.observe(Frame('C-CAN',10,0x100,2,b'00'))
        with pytest.raises(ValueError,match='timestamp'):
            d.observe(Frame('C-CAN',9,0x100,2,b'00'))
        assert d.observe(Frame('C-CAN',10,0x100,2,b'00'))[0]['observed']==0


def test_c_cli_is_default_and_records_library_identity(c_library,tmp_path):
    import json,sys
    p=tmp_path/'policy.json';p.write_text(json.dumps(policy()))
    source=tmp_path/'data.csv'
    source.write_text('Interface,Timestamp,Arbitration_ID,DLC,Data,Label\nC-CAN,0,777,2,00 00,DoS\n')
    out=tmp_path/'run'
    cmd=[sys.executable,'-m','IDS.canids','evaluate','--input',str(source),'--policy',str(p),
         '--library',str(c_library),'--output-dir',str(out)]
    result=subprocess.run(cmd,cwd=ROOT.parent,capture_output=True,text=True,timeout=20)
    assert result.returncode==0,result.stderr
    m=json.loads((out/'manifest.json').read_text())
    assert m['backend']['backend']=='c11'
    assert m['backend']['library_sha256']==CDetector(policy(),c_library).backend_info['library_sha256']
    assert json.loads((out/'report.json').read_text())['metrics']['binary']['tp']==1


def test_replaced_library_path_cannot_reuse_cached_old_abi(c_library,tmp_path):
    active=tmp_path/'active.so';shutil.copyfile(c_library,active)
    stub=tmp_path/'unsupported.c'
    stub.write_text('#include "canids.h"\nuint32_t canids_abi_version(void){return 2;}\n'
                    'size_t canids_frame_size(void){return sizeof(canids_frame);}\n'
                    'size_t canids_profile_size(void){return sizeof(canids_profile);}\n'
                    'size_t canids_finding_size(void){return sizeof(canids_finding);}\n')
    replacement=tmp_path/'replacement.so'
    result=subprocess.run(['cc','-std=c11','-shared','-fPIC','-I',str(ROOT/'include'),str(stub),'-o',str(replacement)],
                          capture_output=True,text=True,timeout=20)
    assert result.returncode==0,result.stderr
    with CDetector(policy(),active):
        replacement.replace(active)
        with pytest.raises(ValueError,match='ABI'):
            CDetector(policy(),active)
