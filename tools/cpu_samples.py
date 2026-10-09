#!/usr/bin/env python3
"""Report of a BB_CPU_SAMPLE file (gpu/shim/bbport_cpu_sampler.cpp).

    cpu_samples.py cpu-samples.txt bb-probe.exe [--thread NAME] [--top N] [--paths N]

Per thread: its share of a core, then the functions where it spent its cycles (self) and the
functions on its stacks (inclusive). bb-probe.exe frames are named from the executable's symbol
table (llvm-nm or nm); other modules are shown as module+offset, guest (PS4) code as "guest".
Weights are the cycles each thread ran before its samples, not sample counts.
"""
import argparse
import bisect
import collections
import shutil
import subprocess
import sys


def load_symbols(exe):
    tool = shutil.which('llvm-nm') or shutil.which('x86_64-w64-mingw32-nm') or 'nm'
    out = subprocess.run([tool, '-C', '--defined-only', exe], capture_output=True, text=True,
                         check=True).stdout
    symbols = []
    for line in out.splitlines():
        parts = line.split(' ', 2)
        if len(parts) == 3 and parts[1] in 'Tt':
            symbols.append((int(parts[0], 16), parts[2]))
    symbols.sort()
    image_base = 0x140000000
    try:
        headers = subprocess.run(['objdump', '-p', exe], capture_output=True, text=True).stdout
        for line in headers.splitlines():
            if line.startswith('ImageBase'):
                image_base = int(line.split()[1], 16)
    except OSError:
        pass
    return [a for a, _ in symbols], [n for _, n in symbols], image_base


def simplify(name):
    # Drop argument lists: the report is per function.
    depth = 0
    out = []
    for ch in name:
        if ch == '(':
            depth += 1
            if depth == 1:
                out.append('()')
            continue
        if ch == ')':
            depth -= 1
            continue
        if depth == 0:
            out.append(ch)
    return ''.join(out)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('samples')
    parser.add_argument('exe')
    parser.add_argument('--thread', action='append', default=[])
    parser.add_argument('--top', type=int, default=25)
    parser.add_argument('--paths', type=int, default=0, help='heaviest call paths to print')
    parser.add_argument('--min-share', type=float, default=3.0, help='threads under this %% are skipped')
    args = parser.parse_args()

    addresses, names, image_base = load_symbols(args.exe)
    modules, threads, stacks = [], {}, collections.defaultdict(list)
    seconds = tsc = 1.0
    with open(args.samples, encoding='utf-8', errors='replace') as file:
        for line in file:
            parts = line.split()
            if not parts or parts[0].startswith('#'):
                continue
            if parts[0] == 'seconds':
                seconds = float(parts[1])
            elif parts[0] == 'tsc_per_second':
                tsc = float(parts[1])
            elif parts[0] == 'module':
                modules.append((int(parts[1], 16), int(parts[2], 16), parts[3]))
            elif parts[0] == 'thread':
                threads[parts[1]] = dict(name=parts[2], samples=int(parts[4]), cycles=int(parts[6]))
            elif parts[0] == 'stack':
                stacks[parts[1]].append((int(parts[2]), int(parts[3]), [int(f, 16) for f in parts[4:]]))
    modules.sort()
    main_module = next((m for m in modules if m[2].lower().endswith('.exe')), None)
    cache = {}

    def name_of(address):
        if address in cache:
            return cache[address]
        result = None
        index = bisect.bisect_right(modules, (address, 1 << 64, '')) - 1
        if index >= 0 and modules[index][0] <= address < modules[index][0] + modules[index][1]:
            base, _, module = modules[index]
            if main_module and module == main_module[2]:
                target = address - base + image_base
                i = bisect.bisect_right(addresses, target) - 1
                result = simplify(names[i]) if i >= 0 else f'{module}+{address - base:x}'
            else:
                result = f'{module}'
        cache[address] = result or 'guest'
        return cache[address]

    total_tsc = seconds * tsc
    order = sorted(threads.items(), key=lambda item: -item[1]['cycles'])
    print(f'{seconds:.0f} s sampled; share of one core per thread:')
    for tid, info in order:
        share = info['cycles'] / total_tsc * 100
        if share >= 0.5:
            print(f'  {share:5.1f}%  {info["name"]} (tid {tid}, {info["samples"]} samples)')
    for tid, info in order:
        share = info['cycles'] / total_tsc * 100
        if args.thread and not any(t in info['name'] for t in args.thread):
            continue
        if not args.thread and share < args.min_share:
            continue
        self_weight, incl_weight = collections.Counter(), collections.Counter()
        paths = collections.Counter()
        total = 0
        for _, weight, frames in stacks[tid]:
            weight = weight or 1
            total += weight
            named = [name_of(f) for f in frames]
            self_weight[named[0]] += weight
            for name in set(named):
                incl_weight[name] += weight
            if args.paths:
                paths[' <- '.join(named[:12])] += weight
        if not total:
            continue
        print(f'\n== {info["name"]} (tid {tid}): {share:.1f}% of a core')
        print('  self:')
        for name, weight in self_weight.most_common(args.top):
            print(f'    {weight / total * 100:5.1f}%  {name}')
        print('  inclusive:')
        for name, weight in incl_weight.most_common(args.top):
            print(f'    {weight / total * 100:5.1f}%  {name}')
        for path, weight in paths.most_common(args.paths):
            print(f'    path {weight / total * 100:5.1f}%  {path}')


if __name__ == '__main__':
    sys.exit(main())
