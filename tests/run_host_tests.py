#!/usr/bin/env python3
"""Compile production RX methods against a fake GDMA ring, with ASan/UBSan.
No ESP-IDF or hardware required. This tests ownership, not DMA timing/cache behavior.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def method(source, name):
    match = re.search(r'^.*UartUhci::' + name + r'\([^;]*?\)\s*(?:const\s*)?\{', source, re.M)
    assert match, name
    depth = 0
    for token in re.finditer(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|[{}]', source[match.start():], re.S):
        if token[0] == '{': depth += 1
        if token[0] == '}':
            depth -= 1
            if depth == 0:
                return source[match.start():match.start() + token.end()]
    raise AssertionError(name)

source = (ROOT / 'src/uart_uhci.cc').read_text()
header = re.sub(r'^#include "[^"\n]+"\n|^#pragma once\n', '', (ROOT / 'include/uart_uhci.h').read_text(), flags=re.M)
header = header.replace('private:', 'public:')
names = ['RemountAndRestartDma', 'HasOutstandingBuffers', 'StartReceive', 'StopReceive',
         'ReturnBuffer', 'RecoverOverflow', 'DeferReturnBuffer', 'ReclaimDeferredBuffers',
         'HandleGdmaRxDone', 'HandleGdmaDescrErr', 'Transmit']
text = (ROOT / 'tests/rx_ownership_test.cpp').read_text()
text = text.replace('// @HEADER@', header).replace('// @METHODS@', '\n\n'.join(method(source, name) for name in names))
with tempfile.TemporaryDirectory(prefix='uart-uhci-test-') as tmp:
    cpp = Path(tmp) / 'test.cpp'
    cpp.write_text(text)
    binary = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++20', '-g', '-O1', '-pthread',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
