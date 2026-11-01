#!/usr/bin/env python3
"""Run the ROM-free crash probe and verify diagnostics and real exit statuses.

POSIX: pass a probe linked with the runtime's crash handlers and --gc-sections.
Windows: link platform/os/tests/crash_probe.cpp and os/windows_crash.cpp;
64-bit also links os/windows_stack.cpp. Run on Windows, not through MSYS bash.
"""
import argparse
import json
import os
import pathlib
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("probe", nargs="?")
parser.add_argument("--build-directory", help="Compile the POSIX probe using an existing game's compile_commands.json")
parser.add_argument("--arch", choices=("32", "64"), default="64")
args = parser.parse_args()
temporary = None
if args.build_directory:
    if sys.platform != "linux":
        parser.error("--build-directory is for Linux; pass a native probe executable on other platforms")
    build = pathlib.Path(args.build_directory)
    commands = json.loads((build / "compile_commands.json").read_text())
    entry = next(command for command in commands if command["file"].endswith("/platform/port_runtime.cpp"))
    temporary = tempfile.TemporaryDirectory(prefix="sms-crash-probe-")
    folder = pathlib.Path(temporary.name)
    root = pathlib.Path(__file__).resolve().parent.parent
    # The build supplies the flags; the sources are this checkout's.
    objects = []
    for source in ("platform/port_runtime.cpp", "platform/os/crash_info.cpp"):
        compile_args = shlex.split(entry["command"])
        compile_args[compile_args.index("-o") + 1] = str(folder / (pathlib.Path(source).stem + ".o"))
        compile_args[compile_args.index(entry["file"])] = str(root / source)
        compile_args.extend(["-ffunction-sections", "-fdata-sections"])
        subprocess.run(compile_args, cwd=entry["directory"], check=True)
        objects.append(compile_args[compile_args.index("-o") + 1])
    args.probe = str(folder / "probe")
    subprocess.run([compile_args[0], "-m" + args.arch, "-std=c++11", "-fno-pie", "-no-pie", "-ffunction-sections", "-fdata-sections",
                    "-Wl,--gc-sections", "-I" + str(root / "src"),
                    str(root / "platform/os/tests/crash_probe.cpp"), *objects,
                    "-pthread", "-ldl", "-o", args.probe], check=True)
if not args.probe:
    parser.error("pass a probe executable or --build-directory")
if os.name == "nt":
    cases = [("access", 0xC0000005), ("heap", 0xC0000374), ("unwind", 0xC00000FF)]
    if args.arch == "64":
        cases.append(("low-stack", 0xC0000005))
else:
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    cases = [("access", signal.SIGSEGV), ("abort", signal.SIGABRT),
             ("thread", signal.SIGSEGV), ("overflow", signal.SIGSEGV)]
if os.name == "nt":
    run = subprocess.run([args.probe, "report"], capture_output=True, text=True, timeout=30)
    assert run.returncode == 0 and "[port] fatal: probe report" in run.stderr, (run.returncode, run.stderr)
    assert "[port] registers:" in run.stderr and "[port] build: " in run.stderr, run.stderr
    print("report: registers, backtrace and context without an exception")
for mode, expected in cases:
    run = subprocess.run([args.probe, mode], capture_output=True, text=True, timeout=15)
    if os.name == "nt":
        assert run.returncode & 0xFFFFFFFF == expected, (mode, run.returncode, run.stderr)
        assert "Windows exception 0x%X" % expected in run.stderr, (mode, run.stderr)
        assert re.search(r"backtrace \(stack .*\n.*#0 0x[0-9A-F]+ .*\n.*#1 0x[0-9A-F]+ ", run.stderr), (mode, run.stderr)
        if mode == "low-stack":
            assert "backtrace stops: entry to the game thread's low stack" in run.stderr, (mode, run.stderr)
    else:
        # macOS exits with 128+signal to avoid Rosetta's fatal-signal exit hang.
        assert run.returncode in (-expected, 128 + expected), (mode, run.returncode, run.stderr)
        assert "fatal signal " + signal.Signals(expected).name in run.stderr, (mode, run.stderr)
        assert re.search(r"pc 0x[1-9A-F][0-9A-F]*", run.stderr), (mode, run.stderr)
    assert re.search(r"exception module .* offset 0x[0-9a-fA-F]+", run.stderr), (mode, run.stderr)
    print("%s: correct status and crash address/module diagnostics" % mode)
if os.name == "nt":
    # A stuck addr2line (a copy of the probe) must not outlive the crash report,
    # nor get the files the game has open: players had old game versions that
    # could not be deleted because addr2line.exe from old crashes held them.
    # A symboliser left running would also hold the output pipe, so the run
    # would time out instead of returning.
    def removed(*files):
        deadline = time.monotonic() + 5
        while True:
            try:
                for file in files:
                    if os.path.exists(file):
                        os.remove(file)
                return True
            except PermissionError:
                if time.monotonic() > deadline:
                    return False
                time.sleep(0.2)
    with tempfile.TemporaryDirectory(prefix="sms-symboliser-") as folder:
        fake = os.path.join(folder, "stuck-addr2line.exe")
        held = os.path.join(folder, "held.iso")
        for ending, expected in (("crash", 0xC0000005), ("exit", 3)):
            shutil.copy(args.probe, fake)
            run = subprocess.run([args.probe, "held", held, ending], capture_output=True, text=True, timeout=40,
                                 env={**os.environ, "SMS_ADDR2LINE": fake})
            assert run.returncode & 0xFFFFFFFF == expected, (ending, run.returncode, run.stderr)
            assert "[probe] symboliser holds no game file" in run.stderr, (ending, run.stderr)
            assert removed(fake, held), (ending, "the symboliser outlived the probe", run.stderr)
            print("symboliser on %s: inherits no game file and does not outlive the probe" % ending)
if temporary:
    temporary.cleanup()
