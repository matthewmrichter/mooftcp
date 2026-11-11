"""mooftcp.py - the MoofTCP plugin for moofctl (MoofAgent repo: docs/REMOTE-HARNESS.md "moofctl plugins").

    moofctl --plugins scripts/moofctl-plugins mooftcp switch mactcp|mooftcp [--lite] [--prefs FILE.bin]
                                                                          [--installer NAME]

Switches the Mac between MacTCP and MoofTCP by running MoofTCP's own installer unattended through MoofAgent:
  1. delete the old MoofTCPInstall-sw-<target>.out; with --prefs push FILE.bin (MacBinary, e.g. from
     mac/installer/mkprefs.sh) as "MoofTCP Prefs" next to the installer;
  2. PUT MoofTCPInstall.cfg: "remove" (mactcp) or "reinstall [lite] [prefs]" (mooftcp; remove then install,
     so a candidate build of any version replaces the installed one; inst_auto.h);
  3. RUN the installer (the Mac file is called NAME, default MoofTCPInstall: MoofAgent needs a name of letters
     and digits), GET its .out and check the RESULT line against EXPECTED below;
  4. restart the Mac if the installer changed anything, and return only once the agent is back;
  5. RUN "MoofDiag sw-<target>" and check the first "stack:" line (MoofTCP <vers> full|lite, or MacTCP).
Exit 0 ok, 2 a wrong result (installer or stack), 3 HUNG (from moofctl: no answer in time), 1 a bad call.
"RAN nostart" counts as ran when a fresh RESULT line is there: the installer can finish before the agent
looks for its .out. The core's names (do_restart, check_run_args, save_run, macbinary, parse_macbinary)
come from moofctl.py, which is __main__ under moofctl; tests set CORE.
"""
import os
import re
import sys

CORE = None                     # the moofctl module; None = find it (moofctl runs as __main__)

CFG_NAME = "MoofTCPInstall.cfg"
OUT_TOOL = "MoofTCPInstall"     # the installer names its output after itself, whatever the Mac file is called
PREFS_NAME = "MoofTCP Prefs"
DEFAULT_INSTALLER = "MoofTCPInstall"

# RESULT texts of the installer: (regex, changed, literal fragment of the C source). `changed`: the System
# Folder changed, so the Mac must restart. The fragment must occur in mac/installer/inst_exec.c (inst_message)
# or installer_main.c: test_mooftcp_plugin.py greps them, so a reworded message breaks that test, not a session.
INSTALLED = (r"MoofTCP( Lite)? (\S+) is installed\.", True, " is installed.")
EXPECTED = {
    "mactcp": [
        (r"MoofTCP was removed and MacTCP was put back in the System Folder\.", True,
         "MoofTCP was removed and MacTCP was put back in the System Folder."),
        (r"MoofTCP is not installed, nothing to remove\.", False,
         "MoofTCP is not installed, nothing to remove."),
    ],
    "mooftcp": [INSTALLED],
}
# the one RESULT the installer itself writes without a valid cfg (installer_main.c): never expected
NO_CFG_RESULT = "Unattended run without a valid MoofTCPInstall.cfg. Nothing was changed."

STACK_RE = re.compile(r"^stack: (MoofTCP|MacTCP|none)\b(.*)$")      # the FIRST such line; later "stack:" lines differ


def _core():
    if CORE is not None:
        return CORE
    for m in (sys.modules.get("__main__"), sys.modules.get("moofctl")):
        if m is not None and hasattr(m, "do_restart") and hasattr(m, "parse_macbinary"):
            return m
    raise RuntimeError("mooftcp plugin: moofctl's core names not found (run it through moofctl.py)")


def lines_of(data):
    return data.decode("mac_roman").replace("\r\n", "\n").replace("\r", "\n").split("\n")


def result_line(text_lines):
    """The text after "RESULT " on the last RESULT line, or None."""
    res = None
    for ln in text_lines:
        if ln.startswith("RESULT "):
            res = ln[7:]
    return res


def verdict(target, lite, result):
    """(ok, changed) for an installer RESULT text of `switch <target>`."""
    if result is None:
        return False, False
    for rx, changed, _ in EXPECTED[target]:
        m = re.fullmatch(rx + r".*", result, re.S)
        if not m:
            continue
        if target == "mooftcp" and bool(m.group(1)) != bool(lite):
            return False, False                     # the other variant got installed
        return True, changed
    return False, False


def stack_line(text_lines):
    """(kind, rest) of the first "stack: MoofTCP|MacTCP|none" line of MoofDiag's output, or (None, "")."""
    for ln in text_lines:
        m = STACK_RE.match(ln)
        if m:
            return m.group(1), m.group(2).strip()
    return None, ""


def stack_ok(target, lite, kind, rest):
    if target == "mactcp":
        return kind == "MacTCP"
    m = re.fullmatch(r"(\S+) (full|lite)", rest)
    return kind == "MoofTCP" and m is not None and m.group(2) == ("lite" if lite else "full")


def cfg_text(target, lite, prefs):
    if target == "mactcp":
        return "remove\r"
    return "reinstall" + (" lite" if lite else "") + (" prefs" if prefs else "") + "\r"


def push_prefs(core, agent, path):
    with open(path, "rb") as f:
        m = core.parse_macbinary(f.read())
    blob = core.macbinary(PREFS_NAME, m["type"], m["creator"], m["flags"], m["data"], m["rsrc"],
                          m["crdat"], m["mddat"])
    agent.push(blob)


def do_switch(core, link, agent, target, lite=False, prefs=None, installer=DEFAULT_INSTALLER,
              timeout=600, back_timeout=240, outdir=".", log=print):
    """Returns (exit code, agent). moofctl's Hung (no answer) is not caught here: moofctl exits 3."""
    rid = "sw-" + target
    try:
        core.check_run_args(installer, rid)
    except ValueError as e:
        log("FAILED: %s" % e)
        return 1, agent
    if target == "mactcp" and (lite or prefs):
        log("FAILED: --lite and --prefs go with `switch mooftcp`")
        return 1, agent
    out_name = "%s-%s.out" % (OUT_TOOL, rid)
    try:
        agent.delete(out_name)                      # never read a stale result
    except core.AgentError:
        pass
    if prefs:
        push_prefs(core, agent, prefs)
        log("pushed %s as %r" % (prefs, PREFS_NAME))
    agent.put(CFG_NAME, cfg_text(target, lite, prefs).encode("ascii"))
    ran, _, pc = agent.run(installer, rid, max_s=int(timeout), timeout=timeout + 120)
    log("%s %s: %s in %.1f s (PC clock)" % (installer, rid, ran["result"], pc["seconds"]))
    try:
        data = agent.get(out_name)
    except core.AgentError as e:
        log("FAILED: no %s: %s" % (out_name, e))
        return 2, agent
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, out_name), "wb") as f:
        f.write(data)
    text = lines_of(data)
    res = result_line(text)
    log("installer: %s" % (text[0] if text else ""))
    log("result:    %s" % (res or "(no RESULT line)"))
    ok, changed = verdict(target, lite, res)
    # "nostart": see the module doc; the RESULT line read above is fresh (the old .out was deleted)
    if ran["result"] not in ("done", "nostart") or not ok:
        log("FAILED: switch %s: the installer did not do it (see %s)" % (target, os.path.join(outdir, out_name)))
        return 2, agent
    if changed:
        agent, secs = core.do_restart(link, agent, back_timeout)
        log("Mac back after %.1f s (PC clock)" % secs)
    else:
        log("nothing changed: no restart")
    ran, data, pc = agent.run("MoofDiag", rid, max_s=120, timeout=240)
    for path in core.save_run(outdir, "MoofDiag", rid, ran, data, pc):
        log("saved %s" % path)
    kind, rest = stack_line(lines_of(data or b""))
    log("MoofDiag: stack: %s" % ((kind + " " + rest) if kind else "(no stack: line)"))
    if ran["result"] != "done" or not stack_ok(target, lite, kind, rest):
        log("FAILED: switch %s: MoofDiag says the stack is not %s" %
            (target, "MacTCP" if target == "mactcp" else "MoofTCP %s" % ("lite" if lite else "full")))
        return 2, agent
    log("switch %s: done" % target)
    return 0, agent


def register(sub):
    p = sub.add_parser("mooftcp", help="switch the Mac between MacTCP and MoofTCP through MoofTCP's installer")
    p.add_argument("action", choices=["switch"])
    p.add_argument("value", choices=["mactcp", "mooftcp"], help="the stack to end up with")
    p.add_argument("--lite", action="store_true", help="install MoofTCP Lite (mooftcp only)")
    p.add_argument("--prefs", metavar="FILE.bin", help="a MoofTCP Prefs file (MacBinary) to put in the System Folder first")
    p.add_argument("--installer", default=DEFAULT_INSTALLER, metavar="NAME",
                   help="the installer's name on the Mac (default %(default)s)")
    p.add_argument("--install-timeout", type=float, default=600, help="PC: HUNG if the installer does not answer")
    p.add_argument("--back-timeout", type=float, default=240, help="PC: HUNG if the Mac is not back this many s after RESTART")


def dispatch(cmd, args, link, agent, log):
    if cmd != "mooftcp":
        return None
    return do_switch(_core(), link, agent, args.value, lite=args.lite, prefs=args.prefs,
                     installer=args.installer, timeout=args.install_timeout, back_timeout=args.back_timeout,
                     outdir=getattr(args, "out", "."), log=log)
