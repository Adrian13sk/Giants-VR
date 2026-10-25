# Completion checks (harness)

Each script drops the hero by a mission objective and uses that objective's own mechanic:
the pop-up bomb (`popup_*`), Delphi's tornado (`tornado_*`; `_base` builds Delphi's base first,
which is what unlocks the tornado in the Reaper battles), and plain fighting with the harness's
auto-aim (`kill_*`). A pass is "Objective N done" and "Mission ... complete" in the log.

    B=<path to Bin>
    build/host/headless_render --data "$B" --script scripts/completion/popup_prison7.txt --out out/c.png
    build/host/headless_render --data "$B" --script scripts/completion/kill_5a_charger.txt --auto-aim --out out/c.png

`kill_*` need `--auto-aim` (Delphi's scripts set GVR_AUTOAIM_HAND=l: she casts with the left hand).
`tornado_rstory2/5` set GVR_BASE=0 to skip the base. These are host checks only, not device tests.
