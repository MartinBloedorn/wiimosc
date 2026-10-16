"""Runs a command for N seconds, prefixing each output line (stdout+stderr) with the elapsed time.

Usage: python timestamp.py [--interrupt] <seconds> <command> [args...]
Useful with a Debug build, where wiiuse prints its debug log, to see where time goes.
With --interrupt, the command is stopped with Ctrl+Break (handled like Ctrl+C by the wiimosc tools, so they shut
down cleanly) instead of being killed; on Windows, Ctrl+C itself can't be sent to a single process.
"""
import signal
import subprocess
import sys
import threading
import time

args = sys.argv[1:]
is_interrupting = args[0] == "--interrupt"
if is_interrupting:
    args = args[1:]
duration = float(args[0])
command = args[1:]

start = time.time()
flags = subprocess.CREATE_NEW_PROCESS_GROUP if is_interrupting and sys.platform == "win32" else 0
process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=1, text=True,
                           errors="replace", creationflags=flags)


def pump():
    for line in process.stdout:
        print(f"{time.time() - start:7.3f} {line.rstrip()}", flush=True)


thread = threading.Thread(target=pump, daemon=True)
thread.start()
time.sleep(duration)
if is_interrupting:
    process.send_signal(signal.CTRL_BREAK_EVENT if sys.platform == "win32" else signal.SIGINT)
    try:
        process.wait(20)
    except subprocess.TimeoutExpired:
        print(f"{time.time() - start:7.3f} (no exit after interrupt, killing)", flush=True)
        process.kill()
else:
    process.kill()
thread.join(2)
print(f"{time.time() - start:7.3f} (exit code {process.wait()})", flush=True)
