"""Compile a directory of a2c output (gen_*.c) with the runtime into one program, the files in parallel.

    python tools/a2c/build.py <dir> <out.exe or out.dll> [extra.c or .cpp ...] [--jobs N] [--stubs]

Needs Visual Studio's clang-cl; set VS to the Build Tools folder if vswhere does not find it. --stubs writes
stubs.c with a trap for every host function the generated code names that no .c file on the command line (or
a2c_host.c) defines. Objects are kept in <dir>/obj and only recompiled when their source is newer.
"""
import os, re, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
FLAGS = (os.environ.get("A2C_DEFINES", "").split()) + ["/nologo", "/O2", "/DNDEBUG", "/D_CRT_SECURE_NO_WARNINGS", "/W0", "/MT", "/fp:precise", "-mavx2", "-mfma",
         "/clang:-ffp-contract=off", "--target=x86_64-pc-windows-msvc", "/I" + HERE]


def vs_env():
    vs = os.environ.get("VS")
    if not vs:
        vw = r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
        vs = subprocess.run([vw, "-latest", "-products", "*", "-property", "installationPath"],
                            capture_output=True, text=True).stdout.strip()
    bat = os.path.join(vs, "VC", "Auxiliary", "Build", "vcvars64.bat")
    tmp = os.path.join(os.environ.get("TEMP", "."), "a2c_env.bat")      # quoting a path with spaces through cmd /c is fragile
    with open(tmp, "w", newline="") as f:
        f.write('@call "%s" >nul 2>nul\r\n@set\r\n' % bat)
    # A shell such as Git Bash drops variables it cannot name, and vcvars needs them to find the Windows SDK.
    base = dict(os.environ)
    base.setdefault("ProgramFiles(x86)", r"C:\Program Files (x86)")
    base.setdefault("ProgramFiles", r"C:\Program Files")
    base.setdefault("CommonProgramFiles(x86)", r"C:\Program Files (x86)\Common Files")
    out = subprocess.run(["cmd", "/c", tmp], capture_output=True, text=True, env=base).stdout
    env = dict(line.split("=", 1) for line in out.splitlines() if "=" in line)
    if "INCLUDE" not in env:
        sys.exit("could not get the Visual Studio environment from %s" % bat)
    clang = os.path.join(vs, "VC", "Tools", "Llvm", "x64", "bin", "clang-cl.exe")
    return env, clang


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    jobs = int(sys.argv[sys.argv.index("--jobs") + 1]) if "--jobs" in sys.argv else max(2, (os.cpu_count() or 4) - 2)
    if "--jobs" in sys.argv:
        args.remove(str(jobs))
    d, exe, extra = args[0], args[1], args[2:]
    gen = sorted(f for f in os.listdir(d) if re.fullmatch(r"gen_\w+\.c", f))
    sources = [os.path.join(d, f) for f in gen] + extra + [os.path.join(HERE, "a2c_rt.c"), os.path.join(HERE, "a2c_host.c")]
    if "--stubs" in sys.argv:
        have = set()
        for s in sources:
            if not os.path.basename(s).startswith("gen_"):
                have |= set(re.findall(r"^void H_(\w+)\(cpu_t", open(s, encoding="utf8").read(), re.M))
        decl = open(os.path.join(d, "gen_decl.h"), encoding="utf8").read()
        want = re.findall(r"^void H_(\w+)\(cpu_t \*c\);", decl, re.M)
        missing = [n for n in want if n not in have]
        with open(os.path.join(d, "stubs.c"), "w", encoding="utf8", newline="\n") as f:
            f.write('#include "a2c_rt.h"\n')
            for n in missing:
                f.write('void H_%s(cpu_t *c) { a2c_trap(c, 0, "host function not provided: %s"); }\n' % (n, n))
        sources.append(os.path.join(d, "stubs.c"))
        print("host functions stubbed with a trap: %d %s" % (len(missing), " ".join(missing)))
    env, clang = vs_env()
    obj = os.path.join(d, "obj")
    os.makedirs(obj, exist_ok=True)
    deps = max([os.path.getmtime(os.path.join(HERE, "a2c_rt.h")), os.path.getmtime(os.path.join(d, "gen_decl.h"))])
    # hand-written sources may include other headers of the directory (the generated ones only gen_decl.h)
    local_deps = max([deps] + [os.path.getmtime(os.path.join(d, f)) for f in os.listdir(d) if f.endswith(".h")])

    def compile_one(src):
        o = os.path.join(obj, os.path.splitext(os.path.basename(src))[0] + ".obj")
        dep = deps if os.path.basename(src).startswith("gen_") else local_deps
        if os.path.exists(o) and os.path.getmtime(o) > max(os.path.getmtime(src), dep):
            return src, 0, "", 0.0
        t = time.time()
        cxx = ["/EHsc", "/std:c++17"] if src.endswith(".cpp") else []
        r = subprocess.run([clang] + FLAGS + cxx + ["/I" + d, "/c", src, "/Fo" + o], env=env, capture_output=True, text=True)
        return src, r.returncode, (r.stdout + r.stderr)[-3000:], time.time() - t

    t0 = time.time()
    failed = 0
    with ThreadPoolExecutor(jobs) as ex:
        for src, rc, log, secs in ex.map(compile_one, sources):
            if rc:
                failed += 1
                print("FAILED %s\n%s" % (src, log))
            elif secs > 60:
                print("  %s took %.0fs" % (os.path.basename(src), secs))
    if failed:
        sys.exit("%d files failed" % failed)
    objs = [os.path.join(obj, os.path.splitext(os.path.basename(s))[0] + ".obj") for s in sources]
    rsp = os.path.join(obj, "link.rsp")
    with open(rsp, "w") as f:
        f.write("\n".join('"%s"' % o for o in objs))
    kind = ["/LD"] if exe.lower().endswith(".dll") else []      # a DLL exports what its sources mark dllexport
    r = subprocess.run([clang, "/nologo", "/MT"] + kind + ["@" + rsp, "/Fe" + exe, "/link", "/STACK:67108864",
                        "/MAP:" + os.path.splitext(exe)[0] + ".map"], env=env, capture_output=True, text=True)
    if r.returncode:
        sys.exit("link failed\n" + (r.stdout + r.stderr)[-4000:])
    print("built %s in %.0fs" % (exe, time.time() - t0))


if __name__ == "__main__":
    main()
