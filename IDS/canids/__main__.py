"""External profile generation and completed-run analysis; no live IDS runtime."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys

from .analysis import evaluate
from .model import label_family
from .profile import ProfileBuilder
from .source import Source, file_sha256


def runtime_identity():
    directory = Path(__file__).resolve().parent
    try:
        commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=directory,
                                         text=True, stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError):
        commit = None
    hashes = {p.name: file_sha256(p) for p in sorted(directory.glob('*.py'))}
    for path in [*sorted((directory.parent/'src').glob('*.[ch]')), *sorted((directory.parent/'include').glob('*.h')), directory.parent/'CMakeLists.txt']:
        hashes[str(path.relative_to(directory.parent))] = file_sha256(path)
    return {'git_commit': commit,
            'implementation_sha256': hashes,
            'available_cpu_count': len(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else os.cpu_count()}


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



def main():
    parser=argparse.ArgumentParser(description='External CAN IDS analysis. Run IDS/build/canids directly for detection.')
    commands=parser.add_subparsers(dest='mode',required=True)
    for mode in ('fit','evaluate'):
        p=commands.add_parser(mode)
        p.add_argument('--input',required=True,help='AutoHack Interface CSV or ZIP with truth labels')
        p.add_argument('--member',help='exact CSV member name for ZIP')
        if mode=='fit':
            p.add_argument('--output',required=True,help='new frozen policy JSON')
        else:
            p.add_argument('--run-dir',required=True,help='completed standalone C detection directory')
            p.add_argument('--policy',required=True)
            p.add_argument('--output-dir',required=True,help='new external analysis directory')
    args=parser.parse_args()
    def interrupt(signum,frame):
        raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGTERM,interrupt)
    try:
        result=fit(args) if args.mode=='fit' else evaluate(args,runtime_identity())
    except KeyboardInterrupt:
        print('interrupted; partial analysis evidence retained',file=sys.stderr)
        return 130
    except Exception as exc:
        print(f'error: {type(exc).__name__}: {exc}',file=sys.stderr)
        return 2
    print(json.dumps(result,ensure_ascii=False))
    return 0


if __name__=='__main__':
    raise SystemExit(main())
