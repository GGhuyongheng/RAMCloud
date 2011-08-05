#!/usr/bin/env python
# Copyright (c) 2010 Stanford University
#
# Permission to use, copy, modify, and distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

"""Misc utilities and variables for Python scripts."""

import contextlib
import os
import random
import re
import shlex
import signal
import subprocess
import sys
import time

__all__ = ['sh', 'captureSh', 'Sandbox']

def sh(command, bg=False, **kwargs):
    """Execute a local command."""

    kwargs['shell'] = True
    if bg:
        return subprocess.Popen(command, **kwargs)
    else:
        subprocess.check_call(command, **kwargs)

def captureSh(command, **kwargs):
    """Execute a local command and capture its output."""

    kwargs['shell'] = True
    kwargs['stdout'] = subprocess.PIPE
    p = subprocess.Popen(command, **kwargs)
    rc = p.wait()
    if rc:
        raise subprocess.CalledProcessError(rc, command)
    output = p.stdout.read()
    if output.count('\n') and output[-1] == '\n':
        return output[:-1]
    else:
        return output

class Sandbox(object):
    """A context manager for launching and cleaning up remote processes."""
    class Process(object):
        def __init__(self, host, command, kwargs, sonce, proc):
            self.host = host
            self.command = command
            self.kwargs = kwargs
            self.sonce = sonce
            self.proc = proc
    def __init__(self):
        self.processes = []
    def rsh(self, host, command, bg=False, **kwargs):
        """Execute a remote command."""
        if bg:
            sonce = ''.join([chr(random.choice(range(ord('a'), ord('z'))))
                             for c in range(8)])
            # Assumes scripts are at same path on remote machine
            sh_command = ['ssh', host,
                          '%s/regexec' % scripts_path, sonce,
                          os.getcwd(), "'%s'" % command]
            p = subprocess.Popen(sh_command, **kwargs)
            self.processes.append(self.Process(host, command,
                                               kwargs, sonce, p))
            return p
        else:
            sh_command = ['ssh', host,
                          '%s/remoteexec.py' % scripts_path,
                          "'%s'" % command]
            subprocess.check_call(sh_command, **kwargs)
    def __enter__(self):
        return self
    def __exit__(self, exc_type, exc_value, exc_tb):
        with delayedInterrupts():
            killers = []
            for p in self.processes:
                # Assumes scripts are at same path on remote machine
                killers.append(subprocess.Popen(['ssh', p.host,
                                                 '%s/killpid' % scripts_path,
                                                 p.sonce]))
            for killer in killers:
                killer.wait()
        # a half-assed attempt to clean up zombies
        for p in self.processes:
            try:
                p.proc.kill()
            except:
                pass
            p.proc.wait()
    def checkFailures(self):
        """Raise exception if any process has exited with a non-zero status."""
        for p in self.processes:
            rc = p.proc.poll()
            if rc is not None and rc != 0:
                raise subprocess.CalledProcessError(rc, p.command)

@contextlib.contextmanager
def delayedInterrupts():
    """Block SIGINT and SIGTERM temporarily."""
    quit = []
    def delay(sig, frame):
        if quit:
            print ('Ctrl-C: Quitting during delayed interrupts section ' +
                   'because user insisted')
            raise KeyboardInterrupt
        else:
            quit.append((sig, frame))
    sigs = [signal.SIGINT, signal.SIGTERM]
    prevHandlers = [signal.signal(sig, delay)
                    for sig in sigs]
    try:
        yield None
    finally:
        for sig, handler in zip(sigs, prevHandlers):
            signal.signal(sig, handler)
        if quit:
            raise KeyboardInterrupt(
                'Signal received while in delayed interrupts section')

# This stuff has to be here, rather than at the beginning of the file,
# because config needs some of the functions defined above.
from config import *
import config
__all__.extend(config.__all__)