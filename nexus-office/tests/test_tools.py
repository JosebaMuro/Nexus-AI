import threading
import time

import pytest

from nexus import paths
from nexus.config import get_config
from nexus.db import DB
from nexus.tools import ProposalManager, Supervisor, ToolContext, ToolRegistry, analyze
from nexus.tools.base import validate_args
from nexus.tools.builtin.core_tools import calculator


def test_calculator_is_safe():
    assert calculator({"expression": "2^10 / 4 + sqrt(81)"}, None)["result"] == 265
    with pytest.raises(ValueError):
        calculator({"expression": "__import__('os').system('echo x')"}, None)


def test_validate_args_coerces_and_reports():
    schema = {"type": "object", "properties": {"n": {"type": "integer"}, "s": {"type": "string", "enum": ["a", "b"]}},
              "required": ["n"]}
    clean, errors = validate_args(schema, {"n": "3", "s": "a"})
    assert clean["n"] == 3 and not errors
    _, errors = validate_args(schema, {"s": "z"})
    assert len(errors) == 2


def make_supervisor():
    reg = ToolRegistry()
    return reg, Supervisor(reg, DB(), get_config())


def test_supervisor_auto_deny_and_approval():
    reg, sup = make_supervisor()
    ctx = ToolContext(agent="test")
    assert sup.execute("calculator", {"expression": "1+1"}, ctx)["result"] == 2
    get_config().set("tools.policy", {"low": "deny", "medium": "ask", "high": "admin"})
    assert "error" in sup.execute("calculator", {"expression": "1+1"}, ctx)
    get_config().set("tools.policy", {"low": "ask", "medium": "ask", "high": "admin"})
    events = []
    ctx.emit = events.append

    def approve_later():
        for _ in range(50):
            if sup.list_pending():
                sup.resolve(sup.list_pending()[0]["id"], True, is_admin=False)
                return
            time.sleep(0.05)

    threading.Thread(target=approve_later).start()
    assert sup.execute("calculator", {"expression": "3*3"}, ctx)["result"] == 9
    assert events and events[0]["type"] == "approval_required"
    assert sup.audit_log(5)[0]["approver"] == "usuario"


def test_admin_level_requires_admin():
    reg, sup = make_supervisor()
    ctx = ToolContext(agent="test")
    result = {}
    t = threading.Thread(target=lambda: result.update(sup.execute("python_exec", {"code": "print(42)"}, ctx)))
    t.start()
    for _ in range(50):
        if sup.list_pending():
            break
        time.sleep(0.05)
    pid = sup.list_pending()[0]["id"]
    with pytest.raises(PermissionError):
        sup.resolve(pid, True, is_admin=False)
    sup.resolve(pid, True, is_admin=True)
    t.join(30)
    assert "42" in result["stdout"]


def test_remote_requests_are_escalated():
    _, sup = make_supervisor()
    assert sup.decision_for("medium", remote=False) == "ask"
    assert sup.decision_for("medium", remote=True) == "admin"
    assert sup.decision_for("low", remote=True) == "auto"


GOOD = '''
TOOL = {"name": "word_count", "description": "Cuenta palabras", "risk": "low",
        "parameters": {"type": "object", "properties": {"text": {"type": "string"}}, "required": ["text"]}}

def run(args, ctx=None):
    return {"words": len(args["text"].split())}
'''
BAD = '''
import subprocess
TOOL = {"name": "evil", "description": "x", "risk": "low"}
def run(args, ctx):
    return subprocess.run(["whoami"], capture_output=True).stdout
'''


def test_static_analysis_flags_danger():
    a = analyze(BAD)
    assert a["suggested_risk"] == "high" and any("subprocess" in w for w in a["warnings"])
    assert analyze(GOOD)["ok"] and analyze(GOOD)["suggested_risk"] == "low"
    assert not analyze("def run(:")["ok"]


def test_proposal_lifecycle_installs_sandboxed_tool():
    reg, sup = make_supervisor()
    pm = ProposalManager(reg, sup)
    prop = pm.create("word_count", "Cuenta palabras", GOOD, author="agente:forjador")
    assert prop["status"] == "pendiente" and reg.get("word_count") is None
    assert pm.test(prop["id"], {"text": "uno dos tres"})["result"] == {"words": 3}
    pm.approve(prop["id"])
    assert (paths.community_tools_dir() / "word_count.py").exists()
    spec = reg.get("word_count")
    assert spec and spec.source == "community" and spec.func is None  # se ejecuta aislado
    assert sup.execute("word_count", {"text": "a b"}, ToolContext(agent="t")) == {"words": 2}


def test_risk_is_raised_on_approval():
    reg, sup = make_supervisor()
    pm = ProposalManager(reg, sup)
    prop = pm.create("evil", "x", BAD)
    pm.approve(prop["id"])
    assert reg.get("evil").risk == "high"
