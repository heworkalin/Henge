#!/usr/bin/env python3
# Turn the engine's page-table-walk diagnostics on or off from inside gdb.
#
#   HENGE_WALK_DBG=stderr     point hv_walk_dbg at stderr
#   HENGE_WALK_DBG=0          disable
#   gdb -p <pid> -batch -ex "set confirm off" -x tools/henge-gdb/setdbg.py
#
# hv_walk_dbg is a FILE* the engine writes PTW traces to; pointing it at the
# engine's own stderr keeps the output in the normal log path. The symbol is
# only reliable when the engine was built with the debug hooks.

exec(open('/data/local/tmp/henge/tsbase.py').read().split('TH = {')[0])

want = arg("HENGE_WALK_DBG", "stderr", str).strip()
if want in ("0", "off", "none", ""):
    gdb.execute("set var hv_walk_dbg = (void *)0")
    print("hv_walk_dbg = %s (disabled)" % gdb.parse_and_eval("hv_walk_dbg"))
else:
    tried = []
    for expr in ("stderr", "&__sF[2]", "(void *)stderr", "(FILE *)&__sF[2]"):
        try:
            gdb.execute("set var hv_walk_dbg = %s" % expr)
            tried.append(expr)
            break
        except Exception as e:
            tried.append("%s FAILED (%s)" % (expr, e))
    print("hv_walk_dbg = %s" % gdb.parse_and_eval("hv_walk_dbg"))
    for t in tried:
        print("   %s" % t)
