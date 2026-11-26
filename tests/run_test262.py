"""Runs the ECMAScript conformance suite (test262) against the engine.

    python tests/run_test262.py [path filter ...] [--jobs N] [--save name]

The suite is not part of this repository. Get it once with:
    git clone --depth 1 https://github.com/tc39/test262.git third_party/test262
Filters are substrings of the test path, e.g. `language/expressions` or `built-ins/Array`. Results are printed
per folder; the failures (with the first line of what went wrong) go to out/test262-<name>.txt.
"""
import concurrent.futures, os, re, shutil, subprocess, sys, tempfile

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
suite = os.path.join(root, "third_party", "test262")
exe = os.path.join(root, "build", "Release", "tinybrowser.exe")

HOST = r"""
var $262 = {
  global: globalThis,
  evalScript: function (code) { return (0, eval)(code); },
  gc: function () {},
  detachArrayBuffer: function (b) { if (typeof b.transfer === 'function') { b.transfer(); return null; } throw new Test262Error('detachArrayBuffer is not supported'); },
  createRealm: function () { throw new Test262Error('createRealm is not supported'); },
  agent: { start: function () { throw new Test262Error('agents are not supported'); } }
};
"""


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


harness_cache = {}


def harness(name):
    if name not in harness_cache:
        harness_cache[name] = read(os.path.join(suite, "harness", name))
    return harness_cache[name]


def meta(src):
    m = re.search(r"/\*---(.*?)---\*/", src, re.S)
    info = {"includes": [], "flags": [], "negative": None, "features": []}
    if not m:
        return info
    y = m.group(1)

    def items(key):
        f = re.search(r"^" + key + r":\s*\[(.*?)\]", y, re.M | re.S)
        if f:
            return [x.strip() for x in f.group(1).split(",") if x.strip()]
        f = re.search(r"^" + key + r":\s*\n((?:\s+-\s*.*\n?)+)", y, re.M)
        return [x.strip()[1:].strip() for x in f.group(1).splitlines() if x.strip()] if f else []

    info["includes"], info["flags"], info["features"] = items("includes"), items("flags"), items("features")
    n = re.search(r"^negative:\s*\n((?:\s+.*\n?)+)", y, re.M)
    if n:
        t = re.search(r"type:\s*(\w+)", n.group(1))
        info["negative"] = t.group(1) if t else "Error"
    return info


def build(path):
    src = read(path)
    info = meta(src)
    if "raw" in info["flags"]:
        return src, info
    parts = [HOST, harness("assert.js"), harness("sta.js")]
    if "async" in info["flags"]:
        parts.append(harness("doneprintHandle.js"))
    for inc in info["includes"]:
        parts.append(harness(inc))
    body = "\n".join(parts) + "\n" + src
    if "onlyStrict" in info["flags"]:
        body = '"use strict";\n' + body
    return body, info


def run_chunk(tests):
    """tests: list of relative paths. Returns {path: (passed, reason)}."""
    results = {}
    tmp = tempfile.mkdtemp(prefix="t262_")
    try:
        todo = []
        for i, rel in enumerate(tests):
            try:
                body, info = build(os.path.join(suite, "test", rel))
            except Exception as e:  # a harness file that does not exist, unreadable test...
                results[rel] = (False, "runner: " + str(e))
                continue
            if "module" in info["flags"]:
                results[rel] = (None, "module tests are not run yet")
                continue
            f = os.path.join(tmp, "%d.js" % i)
            with open(f, "w", encoding="utf-8", newline="\n") as out:
                out.write(body)
            todo.append((rel, f, info))
        while todo:
            listing, outp = os.path.join(tmp, "list.txt"), os.path.join(tmp, "out.txt")
            with open(listing, "w", encoding="utf-8", newline="\n") as lf:
                lf.write("\n".join(f for _, f, _ in todo))
            if os.path.exists(outp):
                os.remove(outp)
            try:
                subprocess.run([exe, "--js-batch", listing, outp], timeout=60 + 12 * len(todo))
            except subprocess.TimeoutExpired:
                pass
            text = read(outp) if os.path.exists(outp) else ""
            blocks = re.split(r"^### (?!end [01]$)", text, flags=re.M)[1:]
            done = 0
            for (rel, f, info), block in zip(todo, blocks):
                lines = block.splitlines()
                end = [l[4:] for l in lines if l in ("### end 0", "### end 1")]
                output = [l for l in lines[1:] if l not in ("### end 0", "### end 1")]
                if not end:  # the engine died while running this one
                    results[rel] = (False, "CRASH or hang")
                    done += 1
                    break
                failed = end[-1].strip() == "end 1"
                first = next((l for l in output if l.startswith("Uncaught")), output[0] if output else "")
                if info["negative"]:
                    ok = failed and ("Uncaught " + info["negative"]) in "\n".join(output)
                    results[rel] = (ok, "" if ok else "expected " + info["negative"] + ", got: " + (first or "no error"))
                elif "async" in info["flags"]:
                    ok = not failed and any("Test262:AsyncTestComplete" in l for l in output)
                    results[rel] = (ok, "" if ok else (first or "the test never reported completion"))
                else:
                    results[rel] = (not failed, "" if not failed else first)
                done += 1
            if done == 0:  # no output at all: blame the first and move on
                results[todo[0][0]] = (False, "CRASH or hang")
                done = 1
            todo = todo[done:]
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return results


def main():
    args = sys.argv[1:]
    jobs, name, filters = max(1, (os.cpu_count() or 4) - 2), "all", []
    while args:
        a = args.pop(0)
        if a == "--jobs":
            jobs = int(args.pop(0))
        elif a == "--save":
            name = args.pop(0)
        else:
            filters.append(a.replace("\\", "/"))
    if not os.path.isdir(suite):
        sys.exit("test262 is not downloaded; see the top of this file")
    tests = []
    base = os.path.join(suite, "test")
    for d, _, files in os.walk(base):
        for f in files:
            if not f.endswith(".js") or "_FIXTURE" in f:
                continue
            rel = os.path.relpath(os.path.join(d, f), base).replace("\\", "/")
            if rel.startswith(("intl402/", "staging/", "harness/")) and not filters:
                continue
            if not filters or any(x in rel for x in filters):
                tests.append(rel)
    tests.sort()
    chunks = [tests[i:i + 150] for i in range(0, len(tests), 150)]
    results = {}
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for n, r in enumerate(pool.map(run_chunk, chunks)):
            results.update(r)
            if n % 20 == 19:
                print("  ... %d of %d" % (len(results), len(tests)), flush=True)
    groups = {}
    for rel, (ok, _) in results.items():
        parts = rel.split("/")
        key = "/".join(parts[:3]) if len(filters) else "/".join(parts[:2])
        g = groups.setdefault(key, [0, 0, 0])
        g[0 if ok else 2 if ok is None else 1] += 1
    passed = sum(g[0] for g in groups.values())
    failed = sum(g[1] for g in groups.values())
    skipped = sum(g[2] for g in groups.values())
    os.makedirs(os.path.join(root, "out"), exist_ok=True)
    with open(os.path.join(root, "out", "test262-%s.txt" % name), "w", encoding="utf-8") as f:
        for key in sorted(groups):
            g = groups[key]
            f.write("%-50s %6d pass %6d fail %5d skipped\n" % (key, g[0], g[1], g[2]))
        f.write("\nTOTAL %d pass, %d fail, %d skipped (%.1f%% of those run)\n\n" % (passed, failed, skipped, 100.0 * passed / max(1, passed + failed)))
        for rel in sorted(results):
            ok, why = results[rel]
            if ok is False:
                f.write("FAIL %s\n     %s\n" % (rel, why[:300]))
    for key in sorted(groups):
        g = groups[key]
        print("%-50s %6d pass %6d fail %5d skipped" % (key, g[0], g[1], g[2]))
    print("TOTAL %d pass, %d fail, %d skipped (%.1f%% of those run)" % (passed, failed, skipped, 100.0 * passed / max(1, passed + failed)))


if __name__ == "__main__":
    main()
