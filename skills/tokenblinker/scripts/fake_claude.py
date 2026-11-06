#!/usr/bin/env python3
"""Stand-in for claude, used by `tokenled.py selftest`: the launcher under test starts this instead of Claude.

It writes a fixed, awkward byte stream (binary bytes, CRLF, a line longer than the launcher's parse limit,
a compact `result` line), then, like claude, keeps running until its input closes, echoes that input,
writes a tail without a newline and exits with EXIT_CODE. The self-test compares what comes out of the
launcher with payload() byte for byte. The `result` line makes a working launcher send "D 0" (turn
finished) to the LED; no tokens are counted.
"""
import sys

EXIT_CODE = 7
RESULT = b'{"type":"result","subtype":"success","is_error":false,"result":"tokenblinker selftest"}\n'


def head():
    return (b'{"type":"system","subtype":"init","session_id":"tokenblinker-selftest"}\n'
            b"bytes \x00\x01\xff\xfe h\xc3\xa9llo \xe2\x9c\x93 crlf\r\n"
            + b"x" * 100000 + b"\n" + RESULT)


def tail(stdin):
    return b"stdin:" + stdin.hex().encode() + b"\n" + b"tail-without-newline"


def payload(stdin):
    return head() + tail(stdin)


def main():
    out = sys.stdout.buffer
    out.write(head())
    out.flush()
    stdin = sys.stdin.buffer.read()   # runs until the input closes, like a real session
    out.write(tail(stdin))
    out.flush()
    return EXIT_CODE


if __name__ == "__main__":
    sys.exit(main())
