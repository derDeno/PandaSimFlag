"""Run the real telemetry/rendering functions on the host, without ESP32 hardware.

Requires a C++ compiler and ArduinoJson installed by `pio run`.
"""

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default=shutil.which('clang++') or shutil.which('cl'))
    args = parser.parse_args()
    if not args.compiler:
        parser.error('Pass --compiler with the path to clang++, g++ or cl')
    firmware = Path(__file__).resolve().parents[1]
    source = (firmware / 'src/main.cpp').read_text(encoding='utf-8')
    # Compile production functions unchanged, replacing only the platform types/I/O.
    functions = source[source.index('void drawRain('):source.index('uint32_t readLe32(')]
    functions += source[source.index('void applyNativeTelemetry('):source.index('void handleGt7Packet(')]
    functions += source[source.index('void animate('):source.index('void clearTimedOutDisplay(')]
    with tempfile.TemporaryDirectory(prefix='pandasimflag-telemetry-') as directory:
        output = Path(directory)
        (output / 'telemetry_under_test.h').write_text(functions, encoding='utf-8')
        executable = output / 'check_telemetry.exe'
        includes = [output, firmware / '.pio/libdeps/pandasimflag/ArduinoJson/src']
        test = firmware / 'tests/check_telemetry.cpp'
        if Path(args.compiler).stem.lower() == 'cl':
            command = [args.compiler, '/nologo', '/std:c++17', '/EHsc',
                       *[f'/I{path}' for path in includes], str(test), f'/Fe{executable}']
        else:
            command = [args.compiler, '-std=c++17', *[f'-I{path}' for path in includes],
                       str(test), '-o', str(executable)]
        subprocess.run(command, cwd=output, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
