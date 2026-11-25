"""Run from repository root: python3 -m IDS.canids --help."""
import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys
import time
import uuid
from zoneinfo import ZoneInfo

from .engine import Detector
from .c_engine import CDetector
from .evaluate import Metrics
from .model import label_family
from .profile import ProfileBuilder, validate_policy
from .source import Source, file_sha256

LIMITATIONS = [
    'Experimental observed-envelope policy; not an OEM specification or complete allowlist.',
    'Normal-labelled frames can include reactions to attacks; test data was inspected during EDA.',
    'No signal, checksum, counter, suspension or UDS authorization inference.',
    'Per-frame injection truth; no attack episode labels or one-second detection guarantee.',
    'Window and interval alerts can also attach to normal frames next to injection.',
    'Exact ordered frame hash detects full calibration reuse, not arbitrary overlapping subsets.',
    'File input only; no CAN transmission, dashboard or prevention action.',
]


def now():
    return datetime.now(ZoneInfo('Asia/Seoul')).isoformat()


def runtime_identity():
    directory = Path(__file__).resolve().parent
    try:
        commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=directory,
                                         text=True, stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError):
        commit = None
    hashes = {p.name: file_sha256(p) for p in sorted(directory.glob('*.py'))}
    for relative in ('src/canids.c', 'include/canids.h', 'CMakeLists.txt'):
        hashes[relative] = file_sha256(directory.parent / relative)
    return {'git_commit': commit,
            'implementation_sha256': hashes,
            'available_cpu_count': len(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else os.cpu_count()}


def write_json(path, data):
    """Atomic replacement within a newly reserved run directory."""
    tmp = path.with_name(path.name + '.tmp')
    with tmp.open('w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False, indent=2, allow_nan=False)
        f.write('\n')
    os.replace(tmp, path)


def fit(args):
    output = Path(args.output)
    if output.exists():
        raise FileExistsError(f'policy already exists: {output}')
    identity = runtime_identity()
    builder = ProfileBuilder()
    with Source(args.input, args.member) as source:
        for record in source:
            builder.observe(record.frame, label_family(record.label) == 'Normal')
    if identity['implementation_sha256'] != runtime_identity()['implementation_sha256']:
        raise ValueError('implementation changed during fit')
    policy = builder.finish({**source.metadata, 'producer': identity})
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open('x', encoding='utf-8') as f:
        json.dump(policy, f, ensure_ascii=False, indent=2, allow_nan=False)
        f.write('\n')
    return {'policy': str(output), 'policy_hash': policy['policy_hash'],
            'normal_frames': builder.normal, 'profiles': len(policy['profiles'])}


def detect(args):
    policy_path = Path(args.policy)
    policy = validate_policy(json.loads(policy_path.read_text()))
    if args.backend == 'python' and args.library:
        raise ValueError('--library applies only to the C backend')
    detector = CDetector(policy, args.library) if args.backend == 'c' else Detector(policy)
    backend = detector.backend_info if args.backend == 'c' else {'backend': 'python_reference'}
    output = Path(args.output_dir)
    output.mkdir(parents=True, exist_ok=False)
    run_id = str(uuid.uuid4())
    started = time.monotonic_ns()
    manifest = {'schema_version': 1, 'experiment_id': run_id, 'status': 'running',
                'mode': args.mode, 'started_at': now(), 'policy_hash': policy['policy_hash'],
                'policy_file_sha256': file_sha256(policy_path), 'source': None,
                'python': platform.python_version(), 'kernel': platform.release(),
                'command': [sys.executable, '-m', 'IDS.canids', *sys.argv[1:]],
                'can_transmission_executed': False, 'prevention_enabled': False,
                'backend': backend, **runtime_identity()}
    source = None
    frames = events = alerted_frames = 0
    metrics = Metrics() if args.mode == 'evaluate' else None
    try:
        write_json(output / 'manifest.json', manifest)
        with Source(args.input, args.member) as source, (output / 'ids-events.jsonl').open('x', encoding='utf-8') as sink:
            for record in source:
                # Validate ground truth independently; it is never passed to Detector.
                if metrics is not None:
                    label_family(record.label)
                findings = detector.observe(record.frame)
                frames += 1
                alerted_frames += bool(findings)
                if metrics is not None:
                    metrics.add(record.label, findings, record.frame.bus)
                for finding in findings:
                    events += 1
                    event = {'schema_version': 1, 'event_id': f'{run_id}:{events}', 'experiment_id': run_id,
                             'recorded_at': now(), 'processing_monotonic_ns': time.monotonic_ns(),
                             'timestamp_domain': 'dataset_relative', 'policy_hash': detector.policy_hash,
                             'severity': 'warning', 'action': 'log_only',
                             'source': {'input': str(source.path), 'member': source.member, 'line': record.line},
                             'dlc': record.frame.dlc, 'data': record.frame.data.hex(), **finding}
                    sink.write(json.dumps(event, separators=(',', ':'), ensure_ascii=False) + '\n')
        if not source.metadata['complete']:
            raise ValueError('source did not complete')
        if manifest['implementation_sha256'] != runtime_identity()['implementation_sha256']:
            raise ValueError('implementation changed during detection')
        if args.backend == 'c' and file_sha256(backend['library']) != backend['library_sha256']:
            raise ValueError('C library changed during detection')
        if metrics is not None and not source.metadata['has_labels']:
            raise ValueError('evaluation requires truth labels')
        manifest['policy_unchanged'] = file_sha256(policy_path) == manifest['policy_file_sha256']
        if not manifest['policy_unchanged']:
            raise ValueError('policy file changed during execution')
        duration = (time.monotonic_ns() - started) / 1e9
        same_frames = source.metadata['frames_sha256'] == policy['training']['source'].get('frames_sha256')
        kind = 'calibration_reuse' if same_frames else 'provided_evaluation_input' if metrics else 'unlabelled_detection'
        report = {'schema_version': 1, 'experiment_id': run_id, 'policy_hash': detector.policy_hash,
                  'backend': backend,
                  'evaluation_kind': kind, 'source': source.metadata, 'frames': frames, 'events': events,
                  'alerted_frames': alerted_frames, 'metrics': metrics.report() if metrics is not None else None,
                  'processing_duration_s': duration, 'processing_frames_per_s': frames / duration if duration else None,
                  'episode_detection_latency_s': None, 'limitations': LIMITATIONS}
        write_json(output / 'report.json', report)
        summary = [f'# CAN IDS {args.mode}', '', f'Experiment: {run_id}', f'Policy: {detector.policy_hash}',
                   f'Input role: {kind}', f'Frames: {frames}; alerted frames: {alerted_frames}; events: {events}', '']
        if metrics is not None:
            summary += ['Binary frame metrics:', json.dumps(report['metrics']['binary']), '']
        summary += ['Limitations:', *[f'- {text}' for text in LIMITATIONS], '']
        (output / 'summary.md').write_text('\n'.join(summary), encoding='utf-8')
        manifest.update(status='complete', completed_at=now(), source=source.metadata,
                        frames=frames, events=events,
                        outputs_sha256={n: file_sha256(output / n) for n in ('report.json', 'ids-events.jsonl', 'summary.md')})
        write_json(output / 'manifest.json', manifest)
        return {'output_dir': str(output), 'frames': frames, 'events': events,
                'binary_metrics': report['metrics']['binary'] if metrics else None}
    except BaseException as exc:
        manifest.update(status='interrupted' if isinstance(exc, KeyboardInterrupt) else 'failed',
                        completed_at=now(), frames=frames, events=events,
                        source=source.metadata if source is not None else None,
                        error=f'{type(exc).__name__}: {exc}')
        write_json(output / 'manifest.json', manifest)
        raise
    finally:
        if args.backend == 'c':
            detector.close()


def main():
    parser = argparse.ArgumentParser(description='Passive AutoHack CAN rule IDS; no transmission or prevention')
    commands = parser.add_subparsers(dest='mode', required=True)
    for mode in ('fit', 'detect', 'evaluate'):
        p = commands.add_parser(mode)
        p.add_argument('--input', required=True, help='AutoHack Interface CSV or ZIP')
        p.add_argument('--member', help='exact CSV member name for ZIP input')
        if mode == 'fit':
            p.add_argument('--output', required=True, help='new policy JSON file')
        else:
            p.add_argument('--policy', required=True)
            p.add_argument('--output-dir', required=True, help='new, exclusive run directory')
            p.add_argument('--backend', choices=['c', 'python'], default='c', help='C11 core (default) or Python reference')
            p.add_argument('--library', help='C11 shared library; default IDS/build/libcanids.so')
    args = parser.parse_args()

    def interrupt(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')

    signal.signal(signal.SIGTERM, interrupt)
    try:
        result = fit(args) if args.mode == 'fit' else detect(args)
    except KeyboardInterrupt:
        print('interrupted; partial run evidence retained', file=sys.stderr)
        return 130
    except Exception as exc:
        print(f'error: {type(exc).__name__}: {exc}', file=sys.stderr)
        return 2
    print(json.dumps(result, ensure_ascii=False))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
