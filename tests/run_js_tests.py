"""Runs the JavaScript engine tests: python tests/run_js_tests.py [name ...]"""
import os, subprocess, sys, tempfile

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
exe = os.path.join(root, "build", "Release", "tinybrowser.exe")
jsdir = os.path.join(root, "tests", "js")
harness = open(os.path.join(jsdir, "_harness.js"), encoding="utf-8").read()
names = sys.argv[1:] or sorted(f[:-3] for f in os.listdir(jsdir) if f.endswith(".js") and not f.startswith("_"))
failed = 0
for name in names:
    src = open(os.path.join(jsdir, name + ".js"), encoding="utf-8").read()
    with tempfile.TemporaryDirectory() as tmp:
        script, out = os.path.join(tmp, name + ".js"), os.path.join(tmp, "out.txt")
        # the harness goes on one line so line numbers in stack traces match the test file
        open(script, "w", encoding="utf-8").write(harness.replace("\n", " ") + "\n" + src)
        try:
            code = subprocess.run([exe, "--js", script, out], timeout=120).returncode
        except subprocess.TimeoutExpired:
            code = "timeout"
        text = open(out, encoding="utf-8", errors="replace").read().strip() if os.path.exists(out) else ""
    last = text.splitlines()[-1] if text else ""
    good = code == 0 and last.startswith("ok ")
    failed += not good
    print(("PASS " if good else "FAIL ") + name + "  (exit " + str(code) + ")")
    if not good or "timings" in text:
        print("   " + text.replace("\n", "\n   "))
sys.exit(1 if failed else 0)
