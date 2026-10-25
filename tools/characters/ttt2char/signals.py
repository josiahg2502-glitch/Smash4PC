"""The command line programs end on SIGTERM (the launcher's Cancel) and SIGHUP (a closed terminal) the way Ctrl+C
ends them: by an exception, so that `finally` runs. Python's own action for those signals ends the program at once,
which would leave Tekken 7 running hidden (tekken7/autodump.py's session ends only in its close()) and a pack half
written.

stop_on_signals() (at the start of a program): the first of SIGTERM, SIGHUP and SIGINT raises SystemExit(128 + the
signal) (KeyboardInterrupt for SIGINT, as before); the ones after it are ignored, so that the cleanup it starts runs
to its end. deferred(): a block a signal does not interrupt (the signal's exception comes at its end), for what must
not stop halfway: putting a pack's files in place, ending the hidden session.
"""
import contextlib
import signal

SIGNALS = (signal.SIGTERM, signal.SIGHUP, signal.SIGINT)

_state = {'stopping': None, 'deferred': 0, 'pending': None}


def _stop(signum):
    _state['stopping'] = signum
    if signum == signal.SIGINT:
        raise KeyboardInterrupt
    raise SystemExit(128 + signum)


def _handler(signum, frame):
    if _state['stopping'] is not None:
        return                              # (the first one's cleanup is running)
    if _state['deferred']:
        _state['pending'] = _state['pending'] or signum
        return
    _stop(signum)


def stop_on_signals():
    """Installs the handler (main thread only). The programs it starts get the default actions again (exec resets
    caught signals), so a Cancel that reaches them too still ends them."""
    for s in SIGNALS:
        signal.signal(s, _handler)


def stopping():
    """The signal the program is ending on, or None."""
    return _state['stopping']


@contextlib.contextmanager
def deferred():
    _state['deferred'] += 1
    try:
        yield
    finally:
        _state['deferred'] -= 1
        if not _state['deferred'] and _state['pending'] is not None and _state['stopping'] is None:
            signum, _state['pending'] = _state['pending'], None
            _stop(signum)
