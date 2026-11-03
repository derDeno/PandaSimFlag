"""Hardware smoke test: python tests/check_dual_core.py --host DEVICE_IP --port COM5.

Stop game telemetry first. Requires pyserial (installed with PlatformIO/esptool).
Sends synthetic frames, briefly stalls an HTTP request, and restores device settings.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import http.client
import json
import socket
import threading
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--port', required=True)
    parser.add_argument('--seconds', type=float, default=5)
    args = parser.parse_args()

    def request(path, data=None, raw=None):
        connection = http.client.HTTPConnection(args.host, timeout=6)
        try:
            body = raw if raw is not None else None if data is None else json.dumps(data)
            connection.request('GET' if body is None else 'POST', path, body,
                               {'Content-Type': 'application/json'})
            response = connection.getresponse()
            body = response.read()
            return response.status, json.loads(body) if body else {}
        finally:
            connection.close()

    def state():
        status, result = request('/api/state')
        assert status == 200, (status, result)
        assert (result['controlCore'], result['matrixCore']) == (0, 1), result
        assert result['matrixStackFreeBytes'] > 2048, result['matrixStackFreeBytes']
        return result

    def open_serial():
        # Native USB/JTAG interprets RTS/DTR transitions as reset/boot requests.
        port = serial.Serial(port=None, baudrate=115200, timeout=0.05, write_timeout=2)
        port.dtr = False
        port.rts = False
        port.port = args.port
        port.open()
        return port

    def wait_for(predicate):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            result = state()
            if predicate(result):
                return result
            time.sleep(0.02)
        raise AssertionError('Matrix state did not converge')

    initial = state()
    assert not initial['gameConnected'], 'Stop game telemetry before running this test'
    assert not initial['gameSyncPaused'], 'Press Stop in the Web UI before running this test'
    original = {'hostname': initial['deviceHostname'], 'connectionMode': initial['inputMode'],
                'ledPower': initial['ledPowerPercent'], 'powerLedEnabled': initial['powerLedEnabled'],
                'powerLedBrightness': initial['powerLedBrightness'], 'matrixRotation': initial['matrixRotation']}
    orientation_test_session = None

    def settings(**changes):
        status, result = request('/api/settings', original | changes)
        assert status == 200, (status, result)

    def udp_sender(stop):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            number = 0
            while not stop.is_set():
                display = 'green' if number % 2 else 'blue'
                udp.sendto(json.dumps({'display': display, 'animation': 'none'}).encode(),
                           (args.host, 20777))
                number += 1
                stop.wait(0.01)
            return number

    def read_states(stop):
        count = 0
        while not stop.is_set():
            result = state()
            if result['flag'] in ('green', 'blue'):
                rgb = result['pixels']
                colors = {tuple(rgb[i:i + 3]) for i in range(0, len(rgb), 3)}
                expected = (0, 128, 0) if result['flag'] == 'green' else (0, 0, 255)
                assert colors in ({expected}, {(0, 0, 0)}), (result['flag'], colors)
            count += 1
        return count

    def write_previews(stop):
        count = 0
        while not stop.is_set():
            status, result = request('/api/game', {'source': 'webui', 'display': 'green',
                                                  'animation': 'none'})
            assert status in (204, 503), (status, result)
            count += 1
            stop.wait(0.03)
        return count

    try:
        settings(connectionMode='wifi')
        assert request('/api/game', raw='{bad JSON')[0] == 400
        assert request('/api/game', raw='{}' + ' ' * 1472)[0] == 413
        assert request('/api/game', {'source': 'webui', 'display': 'idle'})[0] == 204
        wait_for(lambda value: value['flag'] == 'idle')
        print('Core affinity, stack margin, HTTP validation and queued preview passed.', flush=True)

        # Calibration owns the physical display, but only Save writes the orientation.
        settings(ledPower=0)
        status, assistant = request('/api/orientation', {'action': 'start'})
        assert status == 200 and assistant['rotation'] == original['matrixRotation'], (status, assistant)
        orientation_test_session = assistant['session']

        def orientation(action, **values):
            return request('/api/orientation', {'action': action, 'session': orientation_test_session, **values})

        assert request('/api/orientation', {'action': 'start'})[0] == 409
        assert request('/api/orientation', {'action': 'cancel', 'session': 0})[0] == 409
        assert orientation('preview', rotation=45)[0] == 400
        assert orientation('preview', rotation='90')[0] == 400
        for rotation in (0, 90, 180, 270):
            assert orientation('preview', rotation=rotation)[0] == 200
            calibrated = wait_for(lambda value: value['orientationActive'] and value['outputBrightness'] > 0)
            assert calibrated['gameSyncPaused'] and calibrated['flag'] == 'orientation', calibrated
            assert calibrated['pixels'] == [255, 0, 0] + [0] * 189, calibrated['pixels']
            assert calibrated['matrixRotation'] == original['matrixRotation']
        assert request('/api/game', {'source': 'webui', 'display': 'idle'})[0] == 409
        assert request('/api/game', {'display': 'green'})[0] == 204
        time.sleep(0.1)
        assert state()['flag'] == 'orientation'
        assert orientation('cancel')[0] == 200
        wait_for(lambda value: not value['orientationActive'] and not value['gameSyncPaused'] and value['outputBrightness'] == 0)
        assert state()['matrixRotation'] == original['matrixRotation']

        status, assistant = request('/api/orientation', {'action': 'start'})
        assert status == 200
        orientation_test_session = assistant['session']
        rotation = (original['matrixRotation'] + 90) % 360
        assert orientation('preview', rotation=rotation)[0] == 200
        assert orientation('save')[0] == 200
        assert wait_for(lambda value: not value['orientationActive'])['matrixRotation'] == rotation
        assert orientation('keepalive')[0] == 409

        status, assistant = request('/api/orientation', {'action': 'start'})
        assert status == 200
        orientation_test_session = assistant['session']
        assert orientation('preview', rotation=(rotation + 90) % 360)[0] == 200
        time.sleep(15.2)
        released = wait_for(lambda value: not value['orientationActive'] and not value['gameSyncPaused'])
        assert released['matrixRotation'] == rotation and released['flag'] == 'idle', released
        assert orientation('save')[0] == 409
        orientation_test_session = None
        settings()
        print('Orientation marker, brightness override, preview/cancel/save, session validation and disconnect timeout passed.', flush=True)

        def manual(display):
            assert request('/api/game', {'source': 'webui', 'display': display,
                                         'animation': 'none'})[0] == 204
            return wait_for(lambda value: value['gameSyncPaused'] == (display != 'idle')
                            and (display == 'idle' or value['flag'] == display))

        manual('red')
        status, preview = request('/api/matrix')
        assert status == 200 and preview['flag'] == 'red' and preview['gameSyncPaused'], preview
        assert 'pixels' not in preview and 'ssid' not in preview and 'uptime' not in preview, preview
        assert preview['animation'] in ('none', 'blink') and 20 <= preview['period'] <= 5000, preview
        stop = threading.Event()
        with ThreadPoolExecutor(max_workers=1) as pool:
            sender = pool.submit(udp_sender, stop)
            try:
                # Both UDP and HTTP game data must leave the manual frame untouched.
                assert request('/api/game', {'display': 'blue'})[0] == 204
                time.sleep(0.2)
                paused = state()
                assert paused['gameSyncPaused'] and paused['flag'] == 'red', paused
                assert paused['pixels'] in ([255, 0, 0] * 64, [0] * 192)
                settings(ledPower=0)
                wait_for(lambda value: value['outputBrightness'] == 0)
                assert state()['gameSyncPaused']
                settings()
                manual('idle')
                wait_for(lambda value: not value['gameSyncPaused'] and value['flag'] in ('green', 'blue'))
            finally:
                stop.set()
                sender.result()
        print('Manual Wi-Fi priority, brightness and Stop/resume passed.', flush=True)

        before = state()['matrixFrames']
        stop = threading.Event()
        with ThreadPoolExecutor(max_workers=1) as pool:
            sender = pool.submit(udp_sender, stop)
            try:
                with open_serial() as port:
                    port.reset_input_buffer()
                    with socket.create_connection((args.host, 80), timeout=3) as slow:
                        slow.sendall(('POST /api/game HTTP/1.1\r\nHost: ' + args.host +
                                      '\r\nContent-Type: application/json\r\nContent-Length: 100\r\n\r\n{').encode())
                        time.sleep(0.2)
                        latencies = []
                        for _ in range(6):
                            port.write(b'\xff' * 6 + b'proto')
                            port.flush()
                            start = time.monotonic()
                            response = b''
                            while time.monotonic() - start < 0.25 and b'SIMHUB_1.0' not in response:
                                response += port.read_until(b'\n', 128)
                            latencies.append(time.monotonic() - start)
                            assert b'SIMHUB_1.0' in response, response
                            time.sleep(0.1)
                        assert max(latencies) < 0.25, latencies
            finally:
                stop.set()
                sent = sender.result()
        after = state()['matrixFrames']
        assert after - before >= 10, (before, after, sent)
        print('USB latency with stalled HTTP, ms:', [round(value * 1000, 2) for value in latencies],
              'matrix frames:', after - before, flush=True)

        stop = threading.Event()
        before = state()['matrixFrames']
        with ThreadPoolExecutor(max_workers=4) as pool:
            futures = [pool.submit(udp_sender, stop), pool.submit(read_states, stop),
                       pool.submit(read_states, stop), pool.submit(write_previews, stop)]
            try:
                time.sleep(args.seconds / 2)
                settings(connectionMode='wifi', powerLedEnabled=False)
                time.sleep(args.seconds / 2)
            finally:
                stop.set()
            counts = [future.result() for future in futures]
        after = state()['matrixFrames']
        assert after - before >= args.seconds * 10, (before, after)
        print('Concurrent UDP/state/preview counts:', counts, 'matrix frames:', after - before, flush=True)

        settings(connectionMode='usb')
        # Changing the source must not release manual control.
        wait_for(lambda value: value['gameSyncPaused'])
        manual('red')
        with open_serial() as port:
            port.write(b'\xff' * 6 + b'sleds' + bytes([4, 0, 0]) * 64 + b'\xff\xfe\xfd')
            port.flush()
            wait_for(lambda value: value['gameConnected'])
            assert state()['pixels'] in ([255, 0, 0] * 64, [0] * 192)
            time.sleep(2.1)
            paused = state()
            assert paused['gameSyncPaused'] and paused['flag'] == 'red', paused
        manual('idle')
        time.sleep(0.05)
        with open_serial() as port:
            port.write(b'\xff' * 6 + b'sleds' + bytes([4, 0, 0]) * 64 + b'\xff\xfe\xfd')
            port.flush()
            frame = wait_for(lambda value: value['flag'] == 'simhub-usb' and value['gameConnected'])
            assert frame['pixels'] == [4, 0, 0] * 64
            status, preview = request('/api/matrix')
            assert status == 200 and preview['pixels'] == frame['pixels'], preview
        time.sleep(2.1)
        idle = wait_for(lambda value: not value['gameConnected'] and value['flag'] == 'idle')
        assert idle['pixels'] == [0] * 192
        print('USB manual priority, timeout protection, Stop and idle clearing passed.', flush=True)
    finally:
        if orientation_test_session is not None:
            request('/api/orientation', {'action': 'cancel', 'session': orientation_test_session})
        if original['connectionMode'] == 'wifi':
            settings(connectionMode='usb')
            wait_for(lambda value: not value['gameConnected'])
        settings()
        request('/api/game', {'source': 'webui', 'display': 'idle'})
        print('Original settings restored; test display cleared.', flush=True)


if __name__ == '__main__':
    main()
