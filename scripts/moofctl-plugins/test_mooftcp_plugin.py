#!/usr/bin/env python3
"""test_mooftcp_plugin.py - scripts/moofctl-plugins/mooftcp.py against a minimal fake agent (written here,
not moofagent's socket-based FakeAgent: the plugin only calls put/get/delete/run/push/restart, and a recorder
shows the order of calls directly). The real moofctl.py supplies the core names (macbinary, do_restart, ...);
find it with $MOOFCTL_PY or ~/projects/moofagent/scripts/moofctl.py (the test is skipped when it is not there)."""
import argparse
import importlib.util
import os
import shutil
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
CORE_PATH = os.environ.get("MOOFCTL_PY") or os.path.expanduser("~/projects/moofagent/scripts/moofctl.py")


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


plug = load("mooftcp_plugin", os.path.join(HERE, "mooftcp.py"))
core = load("moofctl", CORE_PATH) if os.path.exists(CORE_PATH) else None
plug.CORE = core

FULL_OK = "MoofTCP 1.0 is installed. MacTCP was found and moved into the folder \"MacTCP (Disabled)\" in the System Folder; Remove puts it back. Restart to use MoofTCP."
LITE_OK = FULL_OK.replace("MoofTCP 1.0", "MoofTCP Lite 1.0")
BACK_OK = "MoofTCP was removed and MacTCP was put back in the System Folder. Restart to use MacTCP."
NOTHING = "MoofTCP is not installed, nothing to remove."


def out_text(result, variant="full"):
    return ("MoofTCP Installer 1.0 %s\rcfg: reinstall (read and deleted)\rRESULT %s\r" % (variant, result)).encode("mac_roman")


def diag_text(stack):
    return ("MoofDiag at tick 1\rstack: %s\rstack: marks 1 paint 2 low 0 entry 0 used 3\r" % stack).encode("mac_roman")


class Link:
    def __init__(self, log):
        self.log = log

    def open(self, timeout, fresh=False):
        self.log.append("link.open fresh=%s" % fresh)
        return self.agent


class Agent:
    """Records every call in `log`; answers RUN of the installer / MoofDiag from `script`."""

    def __init__(self, log, installer_out, diag_stack, ran_result="done", hang=False):
        self.log, self.installer_out, self.diag_stack = log, installer_out, diag_stack
        self.ran_result, self.hang = ran_result, hang
        self.files = {}

    def delete(self, name):
        self.log.append("delete %s" % name)
        if name not in self.files:
            raise core.AgentError("no such file")
        del self.files[name]

    def push(self, blob):
        m = core.parse_macbinary(blob)
        self.log.append("push %s %s/%s" % (m["name"], m["type"], m["creator"]))
        return m["name"]

    def put(self, name, data):
        self.log.append("put %s %r" % (name, data.decode()))
        self.files[name] = data

    def get(self, name):
        self.log.append("get %s" % name)
        if name not in self.files:
            raise core.AgentError("no such file")
        return self.files[name]

    def run(self, tool, rid, max_s=None, timeout=None):
        self.log.append("run %s %s" % (tool, rid))
        if self.hang:
            raise core.Hung("no answer")
        pc = {"sent": 0.0, "answered": 1.0, "seconds": 1.0}
        if tool == "MoofDiag":
            return {"tool": tool, "id": rid, "result": "done"}, diag_text(self.diag_stack), pc
        if self.installer_out is not None:
            self.files["MoofTCPInstall-%s.out" % rid] = self.installer_out
        return {"tool": tool, "id": rid, "result": self.ran_result}, None, pc

    def restart(self):
        self.log.append("restart")


@unittest.skipIf(core is None, "moofagent's moofctl.py not found (set MOOFCTL_PY)")
class SwitchTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)
        self.log = []
        self.msgs = []

    def switch(self, target, installer_out, stack, **kw):
        agent = Agent(self.log, installer_out, stack, kw.pop("ran_result", "done"), kw.pop("hang", False))
        link = Link(self.log)
        link.agent = agent
        rc, a = plug.do_switch(core, link, agent, target, outdir=self.tmp, log=self.msgs.append, **kw)
        self.assertIs(a, agent)
        return rc

    def calls(self, prefix):
        return [c for c in self.log if c.startswith(prefix)]

    def prefs_file(self):
        p = os.path.join(self.tmp, "prefs.bin")
        with open(p, "wb") as f:
            f.write(core.macbinary("MoofTCP Prefs.bin", "pref", "ztcp", 0, b"", b"R" * 40))
        return p

    def test_mooftcp_happy_path(self):
        rc = self.switch("mooftcp", out_text(FULL_OK), "MoofTCP 1.0 full", prefs=self.prefs_file())
        self.assertEqual(rc, 0, self.msgs)
        order = [self.log.index(self.calls(p)[0]) for p in ("push", "put", "run MoofTCPInstall", "get", "restart", "run MoofDiag")]
        self.assertEqual(order, sorted(order))
        self.assertIn("push MoofTCP Prefs pref/ztcp", self.log)               # renamed on the way
        self.assertIn("put MoofTCPInstall.cfg 'reinstall prefs\\r'", self.log)
        self.assertEqual(len(self.calls("restart")), 1)
        self.assertEqual(self.log.index("link.open fresh=True"), self.log.index("restart") + 1)
        self.assertTrue(os.path.exists(os.path.join(self.tmp, "MoofTCPInstall-sw-mooftcp.out")))
        self.assertTrue(os.path.exists(os.path.join(self.tmp, "MoofDiag-sw-mooftcp.out")))

    def test_lite_cfg_and_variant(self):
        self.assertEqual(self.switch("mooftcp", out_text(LITE_OK, "lite"), "MoofTCP 1.0 lite", lite=True), 0, self.msgs)
        self.assertIn("put MoofTCPInstall.cfg 'reinstall lite\\r'", self.log)

    def test_lite_asked_but_full_installed_is_wrong(self):
        self.assertEqual(self.switch("mooftcp", out_text(FULL_OK), "MoofTCP 1.0 full", lite=True), 2)
        self.assertEqual(self.calls("restart"), [])

    def test_mactcp_when_already_mactcp_does_not_restart(self):
        rc = self.switch("mactcp", out_text(NOTHING), "MacTCP (no MoofTCP status)")
        self.assertEqual(rc, 0, self.msgs)
        self.assertEqual(self.calls("restart"), [])
        self.assertIn("put MoofTCPInstall.cfg 'remove\\r'", self.log)
        self.assertEqual(len(self.calls("run MoofDiag")), 1)                  # still verified

    def test_mactcp_after_mooftcp_restarts(self):
        self.assertEqual(self.switch("mactcp", out_text(BACK_OK), "MacTCP (no MoofTCP status)"), 0, self.msgs)
        self.assertEqual(len(self.calls("restart")), 1)

    def test_result_mismatch_is_exit_2_without_restart(self):
        for res in ("Step 3 of 9 (write MoofTCP) stopped: error -49. Nothing was changed.", plug.NO_CFG_RESULT, None):
            self.log.clear()
            out = out_text(res) if res else b"MoofTCP Installer 1.0 full\r"
            self.assertEqual(self.switch("mooftcp", out, "MoofTCP 1.0 full"), 2)
            self.assertEqual(self.calls("restart"), [])
            self.assertEqual(self.calls("run MoofDiag"), [])

    def test_missing_out_file_is_exit_2(self):
        self.assertEqual(self.switch("mooftcp", None, "MoofTCP 1.0 full"), 2)

    def test_nostart_with_fresh_result_is_accepted(self):
        self.assertEqual(self.switch("mooftcp", out_text(FULL_OK), "MoofTCP 1.0 full", ran_result="nostart"), 0, self.msgs)

    def test_other_run_results_fail(self):
        self.assertEqual(self.switch("mooftcp", out_text(FULL_OK), "MoofTCP 1.0 full", ran_result="timeout"), 2)
        self.assertEqual(self.calls("restart"), [])

    def test_stack_line_is_anchored_to_the_first_one(self):
        # a later "stack: marks ..." line must not be taken, and MacTCP where MoofTCP was wanted fails
        self.assertEqual(self.switch("mooftcp", out_text(FULL_OK), "MacTCP (no MoofTCP status)"), 2)
        self.assertEqual(plug.stack_line(["x", "stack: MoofTCP 1.0rc3 full", "stack: marks 1"]), ("MoofTCP", "1.0rc3 full"))
        self.assertEqual(plug.stack_line(["stack: marks 1 paint 2"]), (None, ""))
        self.assertFalse(plug.stack_ok("mooftcp", False, "MoofTCP", "unconfigured (status view not found)"))

    def test_hung_propagates(self):
        with self.assertRaises(core.Hung):
            self.switch("mooftcp", out_text(FULL_OK), "MoofTCP 1.0 full", hang=True)

    def test_bad_calls(self):
        self.assertEqual(self.switch("mactcp", out_text(NOTHING), "MacTCP", lite=True), 1)
        self.assertEqual(self.switch("mooftcp", out_text(FULL_OK), "x", installer="Moof TCP"), 1)

    def test_register_and_dispatch(self):
        ap = argparse.ArgumentParser()
        sub = ap.add_subparsers(dest="cmd")
        plug.register(sub)
        a = ap.parse_args(["mooftcp", "switch", "mooftcp", "--lite", "--prefs", "p.bin", "--installer", "Inst"])
        self.assertEqual((a.value, a.lite, a.prefs, a.installer), ("mooftcp", True, "p.bin", "Inst"))
        self.assertIsNone(plug.dispatch("other", a, None, None, print))


def slurp(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


class TableTests(unittest.TestCase):
    """The expected RESULT texts come from the C source: a reworded message breaks this test."""

    def test_table_fragments_are_in_the_c_source(self):
        src = slurp("mac/installer/inst_exec.c")
        for target, rows in plug.EXPECTED.items():
            for rx, changed, frag in rows:
                self.assertIn('"%s"' % frag.replace('"', '\\"'), src, "%s: %r not a string in inst_exec.c" % (target, frag))

    def test_regexes_match_what_the_c_source_builds(self):
        self.assertTrue(plug.verdict("mooftcp", False, FULL_OK)[0])
        self.assertTrue(plug.verdict("mooftcp", True, LITE_OK)[0])
        self.assertEqual(plug.verdict("mooftcp", False, FULL_OK), (True, True))
        self.assertEqual(plug.verdict("mactcp", False, BACK_OK), (True, True))
        self.assertEqual(plug.verdict("mactcp", False, NOTHING), (True, False))
        self.assertEqual(plug.verdict("mactcp", False, "MoofTCP was removed. No MacTCP parked by this installer was found, so none was put back."), (False, False))
        self.assertEqual(plug.verdict("mooftcp", False, "MoofTCP is already installed (this version or a newer one). Nothing was changed."), (False, False))

    def test_no_cfg_text_is_in_the_installer(self):
        src = slurp("mac/installer/installer_main.c")
        self.assertIn('"%s"' % plug.NO_CFG_RESULT, src)
        self.assertEqual(plug.verdict("mooftcp", False, plug.NO_CFG_RESULT), (False, False))

    def test_installed_text_has_the_variant_and_version(self):
        src = slurp("mac/installer/inst_exec.c")
        self.assertIn('"MoofTCP Lite"', src)        # vname(): the regex's "( Lite)?"
        self.assertIn('static const char *vname', src)


# moofctl loads every *.py in a plugin directory, this file too: it is a plugin that handles no command.
def register(sub):
    pass


def dispatch(cmd, args, link, agent, log):
    return None


if __name__ == "__main__":
    unittest.main()
