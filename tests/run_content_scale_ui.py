#!/usr/bin/env python3
"""Run full-window scaling checks with private display, bus and offline audio."""
from pathlib import Path
import os
import resource
import select
import shutil
import subprocess
import sys
import tempfile


def binary(name):
    found = shutil.which(name)
    if found:
        return [found]
    host = Path('/run/host/usr')
    loader = host / 'lib/x86_64-linux-gnu/ld-linux-x86-64.so.2'
    executable = host / 'bin' / name
    if loader.is_file() and executable.is_file():
        return [str(loader), '--library-path', str(host / 'lib/x86_64-linux-gnu'), str(executable)]
    return None


def main():
    if len(sys.argv) < 2:
        print('Usage: run_content_scale_ui.py TEST_EXECUTABLE [TEST_ARGS]', file=sys.stderr)
        return 2
    xvfb, dbus = binary('Xvfb'), binary('dbus-daemon')
    if not xvfb or not dbus:
        print('Private scaling check skipped: Xvfb or dbus-daemon is unavailable.', file=sys.stderr)
        return 77
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix='radios-scale-private-') as temporary, \
            tempfile.TemporaryFile() as display_log, tempfile.TemporaryFile() as bus_log:
        runtime = Path(temporary)
        runtime.chmod(0o700)
        environment = dict(os.environ, XDG_RUNTIME_DIR=str(runtime),
                           PULSE_SERVER='unix:' + str(runtime / 'unavailable-audio'),
                           GDK_BACKEND='x11', NO_AT_BRIDGE='1', GTK_A11Y='none',
                           GTK_IM_MODULE='gtk-im-context-simple', G_DEBUG='fatal-warnings',
                           PYTHONDONTWRITEBYTECODE='1', GSETTINGS_BACKEND='memory')
        environment.pop('WAYLAND_DISPLAY', None)
        environment.pop('RADIOS_SDBASE_TOKEN', None)
        for key in ('XDG_CONFIG_HOME', 'XDG_CACHE_HOME', 'XDG_DATA_HOME'):
            destination = runtime / key.lower()
            destination.mkdir(mode=0o700)
            environment[key] = str(destination)
        processes = []
        read_fd, write_fd = os.pipe()
        try:
            try:
                server = subprocess.Popen(xvfb + ['-displayfd', str(write_fd), '-screen', '0',
                    '2560x1600x24', '-nolisten', 'tcp', '-ac'], stdin=subprocess.DEVNULL,
                    stdout=display_log, stderr=display_log, pass_fds=(write_fd,), env=environment)
                processes.append(server)
            finally:
                os.close(write_fd)
            if not select.select([read_fd], [], [], 10)[0]:
                raise RuntimeError('Private Xvfb did not report its display')
            display = os.read(read_fd, 64).decode('ascii').strip()
            if not display.isdigit():
                raise RuntimeError('Private Xvfb failed to start')
            environment['DISPLAY'] = ':' + display
            configuration = runtime / 'session.conf'
            configuration.write_text('<busconfig><type>session</type><listen>unix:tmpdir=' +
                str(runtime) + '</listen><auth>EXTERNAL</auth><policy context="default">' +
                '<allow send_destination="*"/><allow receive_sender="*"/>' +
                '<allow own="*"/></policy></busconfig>')
            bus = subprocess.Popen(dbus + ['--config-file=' + str(configuration), '--nofork',
                '--print-address=1'], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=bus_log, env=environment, text=True)
            processes.append(bus)
            if not select.select([bus.stdout], [], [], 10)[0]:
                raise RuntimeError('Private D-Bus did not report its address')
            address = bus.stdout.readline().strip()
            if not address.startswith('unix:'):
                raise RuntimeError('Private D-Bus failed to start')
            environment['DBUS_SESSION_BUS_ADDRESS'] = address
            return subprocess.run(sys.argv[1:], env=environment, timeout=90, check=False).returncode
        finally:
            os.close(read_fd)
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)


if __name__ == '__main__':
    raise SystemExit(main())
