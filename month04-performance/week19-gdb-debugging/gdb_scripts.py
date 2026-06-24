"""
gdb Python scripts for Week 19
Usage: (gdb) source gdb_scripts.py
       (gdb) warden            # 自定义命令
       (gdb) leak-check        # 检查内存泄漏
       (gdb) p my_var          # pretty printers auto-load
"""

import gdb


# ═══════════════════════════════════════════════════════
# Custom command: warden — checkpoint current state
# ═══════════════════════════════════════════════════════

class Warden(gdb.Command):
    """Warden — print current thread, backtrace, and frame info.

    Usage: warden [label]
      Prints thread ID, backtrace, and local variables.
      Optional label annotates the checkpoint.
    """
    def __init__(self):
        super().__init__('warden', gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        label = arg.strip() if arg else ""
        if label:
            print(f"\n{'='*60}")
            print(f"  [warden] {label}")
            print(f"{'='*60}")
        else:
            print(f"\n{'='*60}")
            print(f"  [warden] checkpoint")
            print(f"{'='*60}")

        try:
            thread = gdb.selected_thread()
            frame = gdb.selected_frame()
            print(f"  Thread: {thread.num}.{thread.ptid[1] or 'main'}")
            print(f"  Frame:  {frame.name()}() at {frame.find_sal().symtab}:{frame.find_sal().line}")
            print()
            print("  --- Backtrace ---")
            gdb.execute('bt 5')
            print("\n  --- Local variables ---")
            gdb.execute('info locals')
        except Exception as e:
            print(f"  [warden error] {e}")


Warden()


# ═══════════════════════════════════════════════════════
# Custom command: leak-check — check container sizes
# ═══════════════════════════════════════════════════════

class LeakCheck(gdb.Command):
    """Check common container sizes for unexpected growth.

    Usage: leak-check
      Iterates all global/static variables and prints
      the size of any recognized containers.
    """
    def __init__(self):
        super().__init__('leak-check', gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        print("\n[leak-check] Scanning for containers...")
        # Simple approach: check known global variables
        for sym_name in ['_accounts', 'g_shared_counter', 'g_request_count']:
            try:
                val = gdb.parse_and_eval(sym_name)
                print(f"  {sym_name} = {val}")
            except Exception:
                pass
        print("[leak-check] Done. Use 'info locals' in each frame for local variables.")


LeakCheck()


# ═══════════════════════════════════════════════════════
# Pretty printers
# ═══════════════════════════════════════════════════════

class Vec3Printer:
    """Pretty print Vec3 as (x, y, z)"""
    def __init__(self, val):
        self.val = val

    def to_string(self):
        x = self.val['x']
        y = self.val['y']
        z = self.val['z']
        return f"Vec3({x}, {y}, {z})"


class BoundingBoxPrinter:
    """Pretty print BoundingBox with min/max"""
    def __init__(self, val):
        self.val = val

    def to_string(self):
        mn = self.val['_min']
        mx = self.val['_max']
        return (f"BoundingBox(min=({mn['x']},{mn['y']},{mn['z']}), "
                f"max=({mx['x']},{mx['y']},{mx['z']}))")


def build_pretty_printer():
    """Register pretty printers for our types"""
    pp = gdb.printing.RegexpCollectionPrettyPrinter("cpp-journey")
    pp.add_printer('Vec3', '^ex7_gdb_python::Vec3$', Vec3Printer)
    pp.add_printer('BoundingBox', '^ex7_gdb_python::BoundingBox$', BoundingBoxPrinter)
    return pp


gdb.printing.register_pretty_printer(gdb.current_objfile(), build_pretty_printer())


# ═══════════════════════════════════════════════════════
# Convenience functions
# ═══════════════════════════════════════════════════════

class AllThreadsBT(gdb.Command):
    """Shortcut: thread apply all bt (print all thread backtraces)"""
    def __init__(self):
        super().__init__('all-bt', gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        gdb.execute('thread apply all bt')


AllThreadsBT()


class DeadlockCheck(gdb.Command):
    """Check all threads for mutex wait patterns suggestive of deadlock.

    Looks at each thread's backtrace for __lll_lock_wait or similar.
    """
    def __init__(self):
        super().__init__('deadlock-check', gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        print("\n[deadlock-check] Checking for blocked threads...")
        for thread in gdb.selected_inferior().threads():
            thread.switch()
            try:
                frame = gdb.newest_frame()
                func_name = frame.name()
                if func_name and any(
                    kw in func_name.lower()
                    for kw in ['lock', 'wait', 'mutex', 'futex', 'pthread']
                ):
                    print(f"\n  Thread {thread.num} BLOCKED in: {func_name}")
                    # Print a compact backtrace
                    bt = gdb.execute('bt 3', to_string=True)
                    for line in bt.split('\n')[1:4]:
                        if line.strip():
                            print(f"    {line.strip()}")
            except Exception:
                pass
        print("\n[deadlock-check] Done.")


DeadlockCheck()


print("[gdb_scripts.py] Loaded. Commands: warden, leak-check, all-bt, deadlock-check")
