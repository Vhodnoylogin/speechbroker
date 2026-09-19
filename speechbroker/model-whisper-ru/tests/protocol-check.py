# The wire between the shim and the child, exercised from the shim's side.
#
#   python tests/protocol-check.py                 run every case
#   python tests/protocol-check.py --take quiet    use another recording
#
# WHY THIS EXISTS. --wav opens the backend and calls Recognise inside the child;
# the pipe takes no part in it at all. In the game the shim lives inside
# SkyrimVR.exe, raises the child as a process of its own and speaks to it in
# frames - a header of sixteen bytes, Hello first, exactly one Reply per Request,
# an exit code for every way of ending. None of that had ever been executed by
# anything until this script was written.
#
# A fault in the frame stream looks, from inside a headset, like nothing
# happening: the shim waits for an answer and the child waits for a frame it can
# recognise. There is no line to read and no way to tell the two apart. Here the
# same fault is a failed case with a name.
#
# This is the shim's side ONLY. How the shim itself raises the child from inside
# the game, the microphone and the path to a subscriber stay for the game run.
#
# Every path comes out of config/build.json and this file's own location, so the
# script is true on any machine that has the module.
import argparse
import array
import io
import json
import os
import struct
import subprocess
import sys
import time
import wave

MAGIC = 0x43564253          # the bytes S B V C
VERSION = 1
HELLO, REQUEST, REPLY, VOCABULARY, CANCEL, LOG, BYE = 1, 3, 4, 5, 6, 7, 8
STATUS_OK, STATUS_CANCELLED, STATUS_FAILED = 1, 5, 6

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


# --- the wire ---------------------------------------------------------------

def frame(kind, body=b''):
    return struct.pack('<IHHII', MAGIC, VERSION, kind, len(body), 0) + body


def put_string(text):
    raw = text.encode('utf-8')
    return struct.pack('<I', len(raw)) + raw


def take_string(body, at):
    (count,) = struct.unpack_from('<I', body, at)
    at += 4
    return body[at:at + count].decode('utf-8', 'replace'), at + count


def read_exactly(pipe, count):
    out = b''
    while len(out) < count:
        piece = pipe.read(count - len(out))
        if not piece:
            return None
        out += piece
    return out


def read_frame(pipe):
    head = read_exactly(pipe, 16)
    if head is None:
        return None
    magic, version, kind, length, reserved = struct.unpack('<IHHII', head)
    if magic != MAGIC:
        raise AssertionError('magic is %08x and not %08x' % (magic, MAGIC))
    if version != VERSION:
        raise AssertionError('version is %d and not %d' % (version, VERSION))
    if reserved != 0:
        raise AssertionError('reserved is %d and not 0' % reserved)
    if length > 67108864:
        raise AssertionError('bodyBytes %d is above the ceiling' % length)
    body = read_exactly(pipe, length) if length else b''
    if body is None:
        raise AssertionError('the body of a frame of type %d never arrived' % kind)
    return kind, body


def parse_hello(body):
    protocol, max_samples = struct.unpack_from('<II', body, 0)
    backend, at = take_string(body, 8)
    model_id, _ = take_string(body, at)
    return {'protocol': protocol, 'maxSamples': max_samples,
            'backend': backend, 'modelId': model_id}


def parse_log(body):
    (level,) = struct.unpack_from('<i', body, 0)
    key, at = take_string(body, 4)
    (count,) = struct.unpack_from('<i', body, at)
    at += 4
    args = []
    for _ in range(count):
        one, at = take_string(body, at)
        args.append(one)
    return {'level': level, 'key': key, 'args': args}


def parse_reply(body):
    utterance, = struct.unpack_from('<q', body, 0)
    serial, status, latency, lost, fragments = struct.unpack_from('<iiiIi', body, 8)
    at = 28
    pieces = []
    for _ in range(fragments):
        start, end, score, ends, last_word, no_speech, gap, words = (
            struct.unpack_from('<iififfii', body, at))
        at += 32
        text, at = take_string(body, at)
        pieces.append({'startMs': start, 'endMs': end, 'score': score,
                       'endsSentence': ends, 'lastWordProb': last_word,
                       'noSpeechProb': no_speech, 'medianGapMs': gap,
                       'words': words, 'text': text})
    failed, at = take_string(body, at)
    return {'utteranceId': utterance, 'serial': serial, 'status': status,
            'latencyMs': latency, 'lostSamples': lost, 'fragments': pieces,
            'failed': failed}


def request_body(utterance_id, turn_id, serial, final, samples):
    head = struct.pack('<qqiiIII', utterance_id, turn_id, serial, 1 if final else 0,
                       0, 0, len(samples))
    return head + samples.tobytes()


# --- the child --------------------------------------------------------------

def settings():
    path = os.path.join(ROOT, 'config', 'build.json')
    with io.open(path, encoding='utf-8') as handle:
        return json.load(handle)['deploy']


def paths(args):
    deploy = settings()
    store = deploy.get('weightsStore', os.path.join(ROOT, 'weights'))
    return {
        'exe': args.child or os.path.join(ROOT, 'child', 'build', 'Release',
                                          'SpeechBrokerWhisperChild.exe'),
        'library': args.library or os.path.join(ROOT, 'child', 'runtime', 'whisper.dll'),
        'weights': args.weights or os.path.join(store, args.set),
        'takes': args.takes or os.path.normpath(
            os.path.join(ROOT, '..', 'tools', 'audiolab', 'takes')),
    }


def start_child(place, parent_pid=None, model_id='whisper-ru'):
    line = [place['exe'],
            '--model-id', model_id,
            '--weights', place['weights'],
            '--library', place['library'],
            '--device', 'cpu',
            '--beam-size', '5',
            '--threads', '0',
            '--language', 'ru',
            '--max-samples', '320000']
    if parent_pid is not None:
        line += ['--parent-pid', str(parent_pid)]
    return subprocess.Popen(line, stdin=subprocess.PIPE, stdout=subprocess.PIPE)


def wait_for_reply(child, logs, seconds=120):
    """Drain whatever arrives until the one Reply. Log frames may come at any
    moment, including while no request is outstanding - that is the whole reason
    the shim reads on a thread of its own."""
    deadline = time.time() + seconds
    while time.time() < deadline:
        got = read_frame(child.stdout)
        if got is None:
            raise AssertionError('the pipe closed before the reply came')
        kind, body = got
        if kind == LOG:
            logs.append(parse_log(body))
            continue
        if kind == REPLY:
            return parse_reply(body)
        raise AssertionError('a frame of type %d arrived where a Reply was due' % kind)
    raise AssertionError('no reply inside %d seconds' % seconds)


def read_wav(path):
    with wave.open(path, 'rb') as handle:
        if handle.getnchannels() != 1:
            raise AssertionError('%s is not mono' % path)
        if handle.getframerate() != 16000:
            raise AssertionError('%s is %d Hz and not 16000' % (path, handle.getframerate()))
        if handle.getsampwidth() != 2:
            raise AssertionError('%s is not 16-bit' % path)
        raw = handle.readframes(handle.getnframes())
    pcm = array.array('h')
    pcm.frombytes(raw)
    return array.array('f', [sample / 32768.0 for sample in pcm])


def silence(seconds=1.0):
    return array.array('f', [0.0] * int(16000 * seconds))


def ended(child, seconds):
    """Wait for the child to leave. Returns its exit code, or None if it stayed."""
    deadline = time.time() + seconds
    while time.time() < deadline:
        if child.poll() is not None:
            return child.returncode
        time.sleep(0.1)
    return None


# --- the cases --------------------------------------------------------------

class Run:
    def __init__(self):
        self.failures = []
        self.notes = []

    def case(self, name, body):
        try:
            note = body()
            print('  ok   %-22s %s' % (name, note or ''))
        except Exception as problem:            # noqa: BLE001 - a case may fail any way it likes
            self.failures.append(name)
            print('  FAIL %-22s %s' % (name, problem))


def main():
    # Recognised speech is the one thing that stays in its own language, so the
    # console has to be able to print it. A Windows console is cp1251 here and
    # would otherwise turn the whole answer into question marks.
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    parser = argparse.ArgumentParser()
    parser.add_argument('--take', default='fireball-ru')
    parser.add_argument('--set', default='whisper-ru-small')
    parser.add_argument('--child')
    parser.add_argument('--library')
    parser.add_argument('--weights')
    parser.add_argument('--takes')
    args = parser.parse_args()
    place = paths(args)

    for what in ('exe', 'library', 'weights'):
        if not os.path.exists(place[what]):
            print('there is no %s: %s' % (what, place[what]))
            return 2
    take = os.path.join(place['takes'], args.take + '.wav')
    if not os.path.exists(take):
        print('there is no take: %s' % take)
        return 2

    print('child   %s' % place['exe'])
    print('weights %s' % place['weights'])
    print('take    %s' % take)
    print()

    run = Run()
    samples = read_wav(take)
    logs = []

    # One child serves the cases that leave the conversation usable.
    child = start_child(place)

    def hello():
        kind, body = read_frame(child.stdout)
        while kind == LOG:
            logs.append(parse_log(body))
            kind, body = read_frame(child.stdout)
        if kind != HELLO:
            raise AssertionError('the first frame is type %d and not Hello' % kind)
        said = parse_hello(body)
        if said['protocol'] != VERSION:
            raise AssertionError('Hello says protocol %d' % said['protocol'])
        if said['modelId'] != 'whisper-ru':
            raise AssertionError('Hello echoes model id %r' % said['modelId'])
        if said['maxSamples'] <= 0:
            raise AssertionError('Hello says maxSamples %d' % said['maxSamples'])
        hello.said = said
        return '%s, model %s, up to %d samples' % (
            said['backend'], said['modelId'], said['maxSamples'])

    run.case('hello', hello)

    def vocabulary():
        words = ['закрой дверь', 'что вокруг', 'проверка связи']
        body = struct.pack('<i', len(words)) + b''.join(put_string(w) for w in words)
        child.stdin.write(frame(VOCABULARY, body))
        child.stdin.flush()
        return '%d phrases sent, no answer is owed' % len(words)

    run.case('vocabulary', vocabulary)

    def speech():
        child.stdin.write(frame(REQUEST, request_body(11, 1, 1, True, samples)))
        child.stdin.flush()
        reply = wait_for_reply(child, logs)
        if reply['utteranceId'] != 11:
            raise AssertionError('reply carries utteranceId %d' % reply['utteranceId'])
        if reply['serial'] != 1:
            raise AssertionError('reply carries serial %d' % reply['serial'])
        if reply['status'] != STATUS_OK:
            raise AssertionError('status %d, failed %r' % (reply['status'], reply['failed']))
        if not reply['fragments']:
            raise AssertionError('no fragments for a take that has speech in it')
        speech.reply = reply
        said = ' | '.join(piece['text'].strip() for piece in reply['fragments'])
        return '%d ms, %d piece(s): %s' % (reply['latencyMs'], len(reply['fragments']), said)

    run.case('request and reply', speech)

    def shape():
        reply = getattr(speech, 'reply', None)
        if reply is None:
            raise AssertionError('there was no reply to look at')
        last_end = -1
        for piece in reply['fragments']:
            if piece['startMs'] > piece['endMs']:
                raise AssertionError('a piece ends before it starts')
            if piece['startMs'] < last_end:
                raise AssertionError('pieces overlap or are out of order')
            last_end = piece['endMs']
            if not (0.0 <= piece['score'] <= 1.0):
                raise AssertionError('score %r is not a probability' % piece['score'])
            for field, floor in (('endsSentence', -1), ('medianGapMs', -1), ('words', -1)):
                if piece[field] < floor:
                    raise AssertionError('%s is %r' % (field, piece[field]))
            for field in ('lastWordProb', 'noSpeechProb'):
                value = piece[field]
                if value != -1.0 and not (0.0 <= value <= 1.0):
                    raise AssertionError('%s is %r - neither a probability nor the sentinel'
                                         % (field, value))
        return 'ordered, no overlap, sentinels lawful'

    run.case('fragment shape', shape)

    def quiet():
        child.stdin.write(frame(REQUEST, request_body(12, 2, 1, True, silence(1.0))))
        child.stdin.flush()
        reply = wait_for_reply(child, logs)
        if reply['utteranceId'] != 12:
            raise AssertionError('reply carries utteranceId %d' % reply['utteranceId'])
        if reply['status'] != STATUS_OK:
            raise AssertionError('status %d for a lawful quiet buffer' % reply['status'])
        # A quiet buffer answered with text is NOT a failure here: it is exactly
        # what the adapter's silence probe exists to record, and the honest thing
        # is to report it rather than to fail the wire for it.
        if reply['fragments']:
            said = ' | '.join(p['text'].strip() for p in reply['fragments'])
            run.notes.append('silence was answered with text: %s' % said)
            return 'answered with %d piece(s) - the model invents' % len(reply['fragments'])
        return 'answered with no fragments, which is the honest answer'

    run.case('one second of silence', quiet)

    def cancel():
        child.stdin.write(frame(REQUEST, request_body(13, 3, 1, True, samples)))
        child.stdin.write(frame(CANCEL, struct.pack('<q', 13)))
        child.stdin.flush()
        reply = wait_for_reply(child, logs)
        if reply['utteranceId'] != 13:
            raise AssertionError('reply carries utteranceId %d' % reply['utteranceId'])
        if reply['status'] not in (STATUS_OK, STATUS_CANCELLED):
            raise AssertionError('status %d after a cancel' % reply['status'])
        return 'answered anyway with status %d - the debt is paid either way' % reply['status']

    run.case('cancel is advisory', cancel)

    def farewell():
        child.stdin.write(frame(BYE))
        child.stdin.flush()
        code = ended(child, 20)
        if code is None:
            child.kill()
            raise AssertionError('the child stayed after Bye')
        if code != 0:
            raise AssertionError('exit code %d after Bye' % code)
        return 'left with 0'

    run.case('bye', farewell)

    # Each ending needs a child of its own: the conversation is over afterwards.
    def closed_stdin():
        one = start_child(place)
        try:
            kind, body = read_frame(one.stdout)
            while kind == LOG:
                kind, body = read_frame(one.stdout)
            one.stdin.close()
            code = ended(one, 20)
            if code is None:
                raise AssertionError('the child stayed after its stdin closed')
            if code != 0:
                raise AssertionError('exit code %d after a closed stdin' % code)
            return 'left with 0'
        finally:
            if one.poll() is None:
                one.kill()

    run.case('closed stdin', closed_stdin)

    def rubbish():
        one = start_child(place)
        try:
            kind, body = read_frame(one.stdout)
            while kind == LOG:
                kind, body = read_frame(one.stdout)
            # A header whose magic is wrong ENDS the conversation: the next bytes
            # would be read as somebody's length, and there is no resynchronising
            # from a frame nobody can identify.
            one.stdin.write(struct.pack('<IHHII', 0xDEADBEEF, VERSION, REQUEST, 0, 0))
            one.stdin.flush()
            code = ended(one, 20)
            if code is None:
                raise AssertionError('the child stayed after an unidentifiable frame')
            if code != 2:
                raise AssertionError('exit code %d, and the protocol says 2' % code)
            return 'left with 2, as the protocol says'
        finally:
            if one.poll() is None:
                one.kill()

    run.case('unidentifiable frame', rubbish)

    def orphan():
        # A parent that exists and can be ended. The child opens it and waits on
        # it: this is the belt to the Job Object's braces, and it is the half that
        # can be checked without the game.
        parent = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(600)'])
        one = start_child(place, parent_pid=parent.pid)
        try:
            kind, body = read_frame(one.stdout)
            while kind == LOG:
                kind, body = read_frame(one.stdout)
            parent.kill()
            parent.wait(timeout=20)
            code = ended(one, 30)
            if code is None:
                raise AssertionError('the child outlived the parent it was told to watch')
            return 'left with %d when the parent went away' % code
        finally:
            if one.poll() is None:
                one.kill()
            if parent.poll() is None:
                parent.kill()

    run.case('the parent goes away', orphan)

    print()
    if logs:
        print('log frames the child sent (keys, never rendered text):')
        for one in logs[:12]:
            print('  level %d  %s  %s' % (one['level'], one['key'], ' | '.join(one['args'])))
        if len(logs) > 12:
            print('  ... and %d more' % (len(logs) - 12))
        print()
    for note in run.notes:
        print('note: %s' % note)
    if run.failures:
        print()
        print('FAILED: %s' % ', '.join(run.failures))
        return 1
    print('every case passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
