"""External evaluation of completed native evidence. Never runs a detector."""
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import time

from .evaluate import Metrics
from .model import RULE_IDS
from .profile import validate_policy
from .source import Source, file_sha256

LIMITATIONS = [
    'Experimental observed-envelope policy; not an OEM specification or complete allowlist.',
    'Normal-labelled frames can include reactions to attacks; test data was inspected during EDA.',
    'No signal, checksum, counter, suspension or UDS authorization inference.',
    'Per-frame injection truth; no attack episode labels or one-second detection guarantee.',
    'Window and interval alerts can also attach to normal frames next to injection.',
    'Exact ordered frame hash detects full calibration reuse, not arbitrary overlapping subsets.',
    'File counts do not establish live SocketCAN loss or real-time/prevention readiness.',
]


def _read(path):
    def invalid(value):
        raise ValueError(f'non-finite JSON value: {value}')
    return json.loads(path.read_text(), parse_constant=invalid)


def _write(path, data):
    tmp=path.with_name(path.name+'.tmp')
    with tmp.open('x',encoding='utf-8') as f:
        json.dump(data,f,ensure_ascii=False,indent=2,allow_nan=False)
        f.write('\n')
    os.replace(tmp,path)


def evaluate(args, identity):
    run=Path(args.run_dir).resolve()
    out=Path(args.output_dir)
    out.mkdir(parents=True,exist_ok=False)
    started=time.monotonic()
    stamp=lambda: datetime.now(timezone.utc).isoformat()
    manifest={'schema_version':1,'mode':'external_evaluate','status':'running',
              'native_run':str(run),'started_at':stamp(),'producer':identity,
              'detection_executed':False}
    _write(out/'manifest.json',manifest)
    try:
        native=_read(run/'manifest.json')
        native_digest=file_sha256(run/'manifest.json')
        if native.get('status')!='complete' or native.get('mode')!='detect' or native.get('backend',{}).get('backend')!='c11_standalone':
            raise ValueError('analysis requires a completed standalone C detection run')
        if not native.get('source',{}).get('complete') or not native.get('policy_unchanged'):
            raise ValueError('native input or policy is incomplete')
        hashes=native.get('outputs_sha256',{})
        expected={'runtime.json','ids-events.jsonl','summary.md'}
        if set(hashes)!=expected:
            raise ValueError('native output digests missing')
        for name in expected:
            if file_sha256(run/name)!=hashes[name]:
                raise ValueError(f'native output digest mismatch: {name}')
        policy_path=Path(args.policy)
        policy_digest=file_sha256(policy_path)
        policy=validate_policy(_read(policy_path))
        if policy['policy_hash']!=native['policy_hash'] or policy_digest!=native['policy_file_sha256']:
            raise ValueError('analysis policy differs from native run')
        runtime=_read(run/'runtime.json')
        metrics=Metrics(); frames=events=alerted=0
        with Source(args.input,args.member) as source, (run/'ids-events.jsonl').open() as event_file:
            iterator=iter(event_file)

            def next_event():
                line=next(iterator,None)
                return json.loads(line) if line is not None else None

            event=next_event()
            for record in source:
                findings=[]
                while event is not None:
                    position=event.get('source',{}).get('line')
                    if type(position) is not int or position<record.line:
                        raise ValueError('event source line is invalid or out of order')
                    if position>record.line:
                        break
                    events+=1
                    f=record.frame
                    if (event.get('schema_version')!=1 or event.get('experiment_id')!=native['experiment_id'] or
                        event.get('event_id')!=f"{native['experiment_id']}:{events}" or
                        event.get('policy_hash')!=policy['policy_hash'] or event.get('action')!='log_only' or
                        event.get('severity')!='warning' or event.get('rule_id') not in RULE_IDS or
                        event['source'].get('input')!=native['source']['input'] or
                        event['source'].get('member')!=native['source']['member'] or
                        event.get('source_timestamp_us')!=f.timestamp_us or event.get('bus')!=f.bus or
                        event.get('can_id')!=f'0x{f.can_id:03X}' or event.get('dlc')!=f.dlc or
                        event.get('data')!=f.data.hex() or 'label' in event):
                        raise ValueError('event identity/frame does not match native input')
                    if any(e['rule_id']==event['rule_id'] for e in findings):
                        raise ValueError('duplicate rule event for one frame')
                    findings.append(event)
                    event=next_event()
                frames+=1; alerted+=bool(findings)
                metrics.add(record.label,findings,record.frame.bus)
            if event is not None:
                raise ValueError('events extend beyond the supplied truth input')
        for key in ('input_sha256','member_sha256','frames_sha256','rows'):
            if source.metadata[key]!=native['source'][key]:
                raise ValueError(f'analysis input differs from C input: {key}')
        logging=runtime['logging']
        if (frames!=native['frames'] or frames!=runtime['frames'] or events!=native['events'] or
            events!=runtime['events'] or alerted!=runtime['alerted_frames'] or
            events!=logging['events_written'] or logging['events_pending'] or logging['events_dropped'] or logging['write_failures']):
            raise ValueError('native counts or recording completion differ')
        for name in expected:
            if file_sha256(run/name)!=hashes[name]:
                raise ValueError('native outputs changed during analysis')
        if file_sha256(run/'manifest.json')!=native_digest or file_sha256(policy_path)!=policy_digest:
            raise ValueError('native manifest or policy changed during analysis')
        kind='calibration_reuse' if source.metadata['frames_sha256']==policy['training']['source']['frames_sha256'] else 'provided_evaluation_input'
        report={'schema_version':1,'experiment_id':native['experiment_id'],'policy_hash':policy['policy_hash'],
                'backend':native['backend'],'evaluation_kind':kind,'source':source.metadata,
                'frames':frames,'events':events,'alerted_frames':alerted,'metrics':metrics.report(),
                'processing_duration_s':runtime['processing_duration_ns']/1e9,
                'processing_frames_per_s':runtime['processing_frames_per_s'],
                'native_runtime':runtime,'native_manifest_sha256':native_digest,
                'analysis_duration_s':time.monotonic()-started,'episode_detection_latency_s':None,
                'limitations':LIMITATIONS}
        _write(out/'report.json',report)
        (out/'summary.md').write_text('\n'.join([
            '# External CAN IDS evaluation','',f'Native run: {run}',f'Input role: {kind}',
            f'Frames: {frames}; alerted frames: {alerted}; events: {events}',
            json.dumps(report['metrics']['binary']), '', *[f'- {v}' for v in LIMITATIONS],'']))
        manifest.update(status='complete',completed_at=stamp(),native_manifest_sha256=native_digest,
                        outputs_sha256={n:file_sha256(out/n) for n in ('report.json','summary.md')})
        _write(out/'manifest.json',manifest)
        return {'output_dir':str(out),'frames':frames,'events':events,'binary_metrics':report['metrics']['binary']}
    except BaseException as exc:
        for name in ('report.json','summary.md'):
            (out/name).unlink(missing_ok=True)
        manifest.update(status='interrupted' if isinstance(exc,KeyboardInterrupt) else 'failed',
                        completed_at=stamp(),error=f'{type(exc).__name__}: {exc}')
        _write(out/'manifest.json',manifest)
        raise
