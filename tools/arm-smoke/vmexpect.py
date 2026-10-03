#!/usr/bin/env python3
"""Drive bin/arm-vm's serial console from a script.

    vmexpect.py [-w SECS] SCRIPT -- [arm-vm options]

SCRIPT lines: "expect TEXT" waits up to -w seconds (default 120) for TEXT
in the console output; "send STRING" writes STRING, with Python escapes
(\\r, \\x03), one character every 20 ms like a typist, so that nothing
outruns the receive FIFO.  Blank lines and lines starting with # are
skipped.  Exits 0 when every expect matched; on a timeout or when QEMU
exits early, prints which line failed and exits 1.  QEMU is stopped at
the end either way.  The console is still teed to logs/arm-vm.log.
"""
import os
import select
import subprocess
import sys
import time


def main():
    args = sys.argv[1:]
    wait = 120.0
    if args[:1] == ['-w']:
        wait = float(args[1])
        args = args[2:]
    if len(args) < 2 or args[1] != '--':
        sys.exit(__doc__)
    script, vmargs = args[0], args[2:]
    steps = []
    with open(script) as f:
        for ln in f:
            ln = ln.rstrip('\n')
            if not ln.strip() or ln.lstrip().startswith('#'):
                continue
            op, _, arg = ln.partition(' ')
            if op == 'send':
                arg = arg.encode().decode('unicode_escape').encode('latin-1')
            elif op != 'expect':
                sys.exit('vmexpect: bad line: %s' % ln)
            steps.append((op, arg))

    vm = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      '..', '..', 'bin', 'arm-vm')
    p = subprocess.Popen([vm, '-t', '0'] + vmargs, stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = b''
    pos = 0
    rc = 0
    try:
        for i, (op, arg) in enumerate(steps):
            if op == 'send':
                for c in arg:
                    p.stdin.write(bytes([c]))
                    p.stdin.flush()
                    time.sleep(0.02)
                continue
            want = arg.encode()
            end = time.time() + wait
            while out.find(want, pos) < 0:
                left = end - time.time()
                if left <= 0:
                    print('\nvmexpect: timeout waiting for %r (step %d)'
                          % (arg, i + 1))
                    rc = 1
                    return rc
                r, _, _ = select.select([p.stdout], [], [], left)
                if not r:
                    continue
                data = os.read(p.stdout.fileno(), 4096)
                if not data:
                    print('\nvmexpect: QEMU exited waiting for %r (step %d)'
                          % (arg, i + 1))
                    rc = 1
                    return rc
                sys.stdout.buffer.write(data)
                sys.stdout.flush()
                out += data
            pos = out.find(want, pos) + len(want)
        print('\nvmexpect: all %d steps passed' % len(steps))
        return rc
    finally:
        p.terminate()
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            p.kill()
        # arm-vm runs QEMU in a pipeline; make sure it is gone.
        subprocess.call(['pkill', '-f', '^qemu-system-aarch64 .*-kernel'],
                        stderr=subprocess.DEVNULL)


if __name__ == '__main__':
    sys.exit(main())
