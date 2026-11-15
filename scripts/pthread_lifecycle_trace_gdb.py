# GIGE-MEL-040-R2 observational pthread lifecycle tracer, loaded by gdb -batch -x.
#
# Breakpoints are placed ONLY on glibc / sanitizer-runtime entry points
# (pthread_create/join/detach/exit, glibc start_thread, sanitizer CheckFailed).
# No breakpoint, read or write targets vendor code. Vendor frames are identified
# only by gdb's ordinary unwinder and dynamic-symbol lookup (as TSan itself does),
# never by disassembly. Breakpoints perturb timing; record that limitation.
import json
import os
import time

import gdb

LOG = open(os.environ["GIGE_MEL_PTHREAD_TRACE"], "x", encoding="utf-8")
MASK = (1 << 64) - 1


def emit(**record):
    record["t_ns"] = time.monotonic_ns()
    record["lwp"] = gdb.selected_thread().ptid[1] if gdb.selected_thread() else None
    LOG.write(json.dumps(record) + "\n")
    LOG.flush()


def reg(name):
    return int(gdb.parse_and_eval("$" + name)) & MASK


def where(pc):
    return gdb.solib_name(pc) or "main-executable"


def stack(limit=10):
    frames = []
    frame = gdb.newest_frame()
    while frame is not None and len(frames) < limit:
        pc = frame.pc()
        frames.append({"function": frame.name(), "object": where(pc), "pc": hex(pc)})
        try:
            frame = frame.older()
        except gdb.error:
            break
    return frames


def symbol(address):
    try:
        return gdb.execute(f"info symbol {address:#x}", to_string=True).strip()
    except gdb.error as error:
        return f"unresolved: {error}"


class Return(gdb.FinishBreakpoint):
    def __init__(self, event, out_pointer):
        super().__init__(gdb.newest_frame(), internal=True)
        self.silent = True
        self.event = event
        self.out_pointer = out_pointer

    def stop(self):
        record = {"event": self.event + "_return", "result": reg("rax") & 0xffffffff}
        if self.out_pointer:
            value = int.from_bytes(gdb.selected_inferior().read_memory(self.out_pointer, 8).tobytes(), "little")
            record["pthread_t"] = hex(value)
        emit(**record)
        return False

    def out_of_scope(self):
        emit(event=self.event + "_return_out_of_scope")


class Entry(gdb.Breakpoint):
    def __init__(self, spec, event):
        super().__init__(spec, internal=False)
        self.silent = True
        self.event = event

    def stop(self):
        pc = reg("rip")
        record = {"event": self.event, "implementation": where(pc),
                  "caller_pthread_self": hex(reg("fs_base")), "stack": stack()}
        out_pointer = None
        if self.event == "pthread_create":
            out_pointer = reg("rdi")
            attr = reg("rsi")
            record.update(start_routine=symbol(reg("rdx")), attr=hex(attr))
            if attr:
                # glibc 2.41 struct pthread_attr: flags at +8, ATTR_FLAG_DETACHSTATE == 1.
                flags = int.from_bytes(gdb.selected_inferior().read_memory(attr + 8, 4).tobytes(), "little")
                record["attr_detached"] = bool(flags & 1)
        elif self.event in ("pthread_detach", "pthread_join"):
            record["pthread_t"] = hex(reg("rdi"))
        elif self.event == "thread_start":
            record["pthread_t"] = hex(reg("rdi"))  # struct pthread * == pthread_t
        elif self.event == "sanitizer_check_failed":
            record["check"] = {"file": gdb.selected_inferior().read_memory(reg("rdi"), 64).tobytes()
                               .split(b"\0")[0].decode(errors="replace"), "line": reg("rsi") & 0xffffffff}
        emit(**record)
        if self.event in ("pthread_create", "pthread_detach", "pthread_join"):
            Return(self.event, out_pointer)
        return False


def exited(event):
    emit(event="inferior_exit", exit_code=getattr(event, "exit_code", None))


def thread_exited(event):
    emit(event="thread_exit", exited_lwp=event.inferior_thread.ptid[1])


gdb.execute("set pagination off")
gdb.execute("set confirm off")
gdb.execute("set breakpoint pending on")
gdb.execute("set print thread-events off")
# gdb shares the inferior's output stream. Its "[Detaching after fork ...]" notice
# (e.g. when the Clang TSan runtime forks its symbolizer) would otherwise split a
# sanitizer CHECK line from its stack; the fail-closed parser then rejects it.
gdb.execute("set print inferior-events off")
gdb.execute("set startup-with-shell off")
gdb.execute("set disable-randomization off")  # keep ordinary ASLR, as outside the debugger
gdb.execute("set debuginfod enabled off")     # never perform network symbol lookups
gdb.execute("handle SIGPIPE SIGUSR1 SIGUSR2 SIG32 SIG33 SIG34 nostop noprint pass")
for spec, name in (("pthread_create", "pthread_create"), ("pthread_detach", "pthread_detach"),
                   ("pthread_join", "pthread_join"), ("pthread_exit", "pthread_exit"),
                   ("start_thread", "thread_start"),
                   ("__sanitizer::CheckFailed", "sanitizer_check_failed")):
    Entry(spec, name)
gdb.events.exited.connect(exited)
if hasattr(gdb.events, "thread_exited"):
    gdb.events.thread_exited.connect(thread_exited)
emit(event="trace_ready", gdb=gdb.VERSION)
gdb.execute("run")
emit(event="trace_finished")
LOG.close()
