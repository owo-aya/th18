"""th18.exe 基础结构查询：入口、导入、字符串、函数规模。"""
import pyghidra

pyghidra.start()

PROJ = r"C:\Users\87804\Documents\playground\th18\toolchains\ghidra-projects\lead"

proj = pyghidra.open_project(PROJ, "th18")
try:
    with pyghidra.program_context(proj, "/th18.exe") as prog:
        fm = prog.getFunctionManager()
        fns = list(fm.getFunctions(True))
        print("FUNCTIONS", len(fns))

        st = prog.getSymbolTable()
        print("--- ENTRY POINTS ---")
        for ep in st.getExternalEntryPointIterator():
            fn = fm.getFunctionAt(ep)
            print("ENTRY", ep, fn.getName() if fn else "(no func)")

        print("--- EXTERNAL IMPORTS (d3d/input/sound/time/win) ---")
        seen = set()
        for s in st.getAllSymbols(True):
            if not s.isExternal():
                continue
            name = s.getName()
            if name in seen:
                continue
            low = name.lower()
            if any(k in low for k in ("d3d", "direct", "input", "sound", "timeget", "queryperformance",
                                      "createfile", "writefile", "readfile", "rand", "messagebox")):
                seen.add(name)
                print("IMP", name)

        print("--- KEYWORD STRINGS ---")
        keyword = ("log.txt", "th18", ".dat", "score", "replay", "config", "thbgm", "title",
                   "Direct", "font", "shader", "menu", "shot")
        data = prog.getListing().getDefinedData(True)
        count = 0
        for d in data:
            v = d.getValue()
            if v is None:
                continue
            try:
                text = str(v)
            except Exception:
                continue
            if len(text) < 3 or len(text) > 200:
                continue
            low = text.lower()
            if any(k in low for k in keyword):
                print("STR", d.getAddress(), repr(text[:120]))
                count += 1
                if count > 120:
                    break

        print("--- BIGGEST FUNCTIONS ---")
        sizes = []
        for f in fns:
            body = f.getBody()
            if body is not None:
                sizes.append((body.getNumAddresses(), f.getEntryPoint(), f.getName()))
        sizes.sort(reverse=True)
        for s, a, n in sizes[:15]:
            print("BIG", a, n, s)
finally:
    proj.close()
