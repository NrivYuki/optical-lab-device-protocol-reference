#!/usr/bin/env python3
"""
h8300-elf-gcc が出したアセンブリ(.s)を読み、フレームポインタ(er6)を設定したあとに退避したレジスタの場所へ、
er6 からの負の位置で読み書きしている関数を探す。

gcc 16.2.0 の H8/300H 向けでは、関数の入口で
    mov.l er6,@-er7 / mov.l er7,er6 / mov.l er4,@-er7 / mov.l er5,@-er7 / sub.l #n,er7
と、フレームポインタを設定してからレジスタを退避するのに、局所変数を @(-8,er6) などに置き、
退避したレジスタを上書きする訳し方をすることがある。これを見つけたら、作るのを止める。

使い方: check-frames.py <.s ファイル>...   見つかれば終了コード 1
"""
import re
import sys

PUSH = re.compile(r'^\s*mov\.l\s+er([0-6]),@-er7\s*$')
ACCESS = re.compile(r'@\((-\d+),er6\)')


def check(path):
    bad = []
    func = None
    state = 0          # 0: 入口を探す 1: er6 を退避した 2: er6 を設定した(以後の push を数える)
    saved = 0
    for line in open(path, encoding='latin1'):
        m = re.match(r'^(_[A-Za-z0-9_.$]+):', line)
        if m:
            func, state, saved = m.group(1), 0, 0
            continue
        if func is None:
            continue
        s = line.strip()
        if state == 0 and s == 'mov.l\ter6,@-er7':
            state = 1
            continue
        if state == 1:
            state = 2 if s == 'mov.l\ter7,er6' else 0
            continue
        if state == 2:
            pm = PUSH.match(line)
            if pm and saved >= 0:
                saved += 1
                continue
            if saved >= 0:
                saved = -saved - 1   # 入口の push の並びが終わった。以後は -saved-1 個として扱う
            n = -saved - 1
            for off in ACCESS.findall(line):
                k = -int(off)
                if k <= 4 * n:
                    bad.append(f'{path}: {func}: @({off},er6) は入口で退避した {n} 個のレジスタ(er6-1 から er6-{4 * n})と重なる: {s}')
    return bad


def main():
    found = []
    for p in sys.argv[1:]:
        found += check(p)
    for f in found:
        print(f)
    sys.exit(1 if found else 0)


if __name__ == '__main__':
    main()
