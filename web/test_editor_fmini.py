#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# web/editor_fmini.html against the firmware's real editor handler: headless Chromium with a fake Web MIDI port whose
# SysEx goes to tests/editor_bridge.c (the C protocol code of the FMini-1 firmware, host build).
#   web/test_editor_fmini.py [bridge-binary] [screenshot-dir]
import json, subprocess, sys, time
from pathlib import Path
from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parent.parent
BRIDGE = sys.argv[1] if len(sys.argv) > 1 else None
SHOTS = Path(sys.argv[2]) if len(sys.argv) > 2 else None
if not BRIDGE:
    BRIDGE = "/tmp/editor_bridge"
    subprocess.run(["cc", "-O1", "-w", "-Ibuild/gen", "-Ifirmware/src", "-o", BRIDGE, "tests/editor_bridge.c", "-lm"], cwd=ROOT, check=True)
import os
proc = subprocess.Popen([BRIDGE], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, cwd=ROOT,
                        env=dict(os.environ, FM_RESTORE_ANSWER="yes"))   # the device's answer to the restore question (the panel is not here)
log = []

def fw(cmd, args=()):
    """a request to the firmware: the reply's bytes after F0 7D 46 4C cmd, without F7 (None: no reply)"""
    line = bytes([0xF0, 0x7D, 0x46, 0x4C, cmd, *args, 0xF7]).hex()
    proc.stdin.write(line + "\n"); proc.stdin.flush()
    r = proc.stdout.readline().strip()
    return list(bytes.fromhex(r)) if r else None

def fa(cmd, args=()):
    """the reply's arguments (after the command byte, without F7)"""
    r = fw(cmd, args)
    return r[5:-1] if r else None

CUT = [False]                                    # True: a firmware that cuts the NAMES reply at 664 bytes (up to 1.7.1)
def on_tx(data):
    log.append(list(data))
    r = fw(data[4], data[5:-1])
    if CUT[0] and data[4] == 10 and r and len(r) > 664:
        r = r[:663] + [0xF7]
    if data[4] == 19 and r and r[6] == 2:        # UP_STORE: the host build has no flash ("kept in RAM"): a real device answers 0
        r[6] = 0
    return r

bad = 0
def check(name, ok):
    global bad
    print(f"editor_fmini: {name:<84}{'ok' if ok else 'FAIL'}")
    bad += 0 if ok else 1

MOCK = """
(() => {
  const mk = (kind) => ({ name: 'FM-1 Midi', state: 'connected', type: kind, open: async () => {}, onmidimessage: null });
  const inp = mk('input'), out = mk('output');
  window.__delay = 0; window.__sent = 0;                    /* a slow device: it answers __delay ms after the request */
  out.send = (d) => { window.__sent++; setTimeout(() => window.__tx(Array.from(d)).then((r) => { if (r && inp.onmidimessage) inp.onmidimessage({ data: new Uint8Array(r) }); }), window.__delay); };
  const acc = { inputs: new Map([['i', inp]]), outputs: new Map([['o', out]]), onstatechange: null };
  window.__acc = acc; window.__inp = inp;
  navigator.requestMIDIAccess = async () => acc;
})();
"""

with sync_playwright() as pw:
    br = pw.chromium.launch()
    pg = br.new_page(viewport={"width": 1100, "height": 820})
    errs = []
    pg.on("pageerror", lambda e: errs.append(str(e)))
    pg.on("console", lambda m: errs.append(m.text) if m.type == "error" else None)
    pg.expose_function("__tx", on_tx)
    pg.add_init_script(MOCK)
    pg.goto((ROOT / "web" / "editor_fmini.html").as_uri())
    check("loads off-line: panel greyed, Connect offered", pg.evaluate("document.body.classList.contains('off')") and pg.inner_text("#connect") in ("Conectar", "Connect"))

    pg.click("#connect")
    pg.wait_for_function("!document.body.classList.contains('off')", timeout=15000)
    check("Connect: handshake, descriptors, names; status shows connected", "FMini-1" in pg.inner_text("#stxt") and not errs)
    check("INFO told the page the patch commands exist", pg.evaluate("__fmini.S.canPatch") is True)
    check("the preset list has the 200 slots (all factory patches)", pg.evaluate("document.querySelectorAll('#preset option').length") >= 200)
    nm = pg.evaluate("__fmini.S.names")
    check("  .. and every one of them has its name (the last: " + (nm[199] if len(nm) > 199 else "?") + ")", len(nm) == 200 and all(len(x) > 0 for x in nm))
    pg.wait_for_timeout(300)

    # a track parameter through a knob (FILT > CUTOFF = P_E0)
    pg.click("#tabs button:has-text('FILT')")
    pe0 = pg.evaluate("__fmini.S.pe0")
    before = fa(31, [0, pe0])
    knob = pg.locator(".knob", has_text="CUTOFF")
    knob.focus()
    v0 = pg.evaluate("__fmini.S.p[__fmini.S.pe0]")
    for _ in range(3):
        pg.keyboard.press("ArrowDown")
    pg.wait_for_timeout(400)
    after = fa(31, [0, pe0])
    d0 = (before[2] | before[3] << 7) - 8192; d1 = (after[2] | after[3] << 7) - 8192
    check("CUTOFF knob (keys): the firmware's P_E0 moved down", d1 < d0 and d1 == pg.evaluate("__fmini.S.p[__fmini.S.pe0]"))
    knob.dblclick()
    pg.wait_for_timeout(300)
    after = fa(31, [0, pe0]); d2 = (after[2] | after[3] << 7) - 8192
    check("double click: back to the parameter's default", d2 == pg.evaluate("__fmini.S.desc['0:' + (__fmini.S.pe0)].def"))

    # drag
    b = knob.bounding_box()
    pg.mouse.move(b["x"] + b["width"] / 2, b["y"] + 30); pg.mouse.down(); pg.mouse.move(b["x"] + b["width"] / 2, b["y"] - 60, steps=8); pg.mouse.up()
    pg.wait_for_timeout(400)
    after = fa(31, [0, pe0]); d3 = (after[2] | after[3] << 7) - 8192
    check("dragging a knob up raises it on the device", d3 > d2)

    # a long drag with a slow device (it answers 70 ms after a request): the page keeps at most one write in flight and the
    # device holds the final value soon after the mouse is released, however long the drag lasted
    import math
    knob.dblclick(); pg.wait_for_timeout(300)                   # (the default, mid-range: room to move both ways)
    kb = knob.bounding_box(); cx = kb["x"] + kb["width"] / 2
    pg.evaluate("window.__delay = 70; window.__sent = 0")
    pg.mouse.move(cx, kb["y"] + 60); pg.mouse.down()
    t_drag = time.monotonic(); n_move = 0
    while time.monotonic() - t_drag < 2.0:                       # 2 s of dragging, one move every ~16 ms (a 60 Hz pointer)
        n_move += 1                                              # (it swings up and down: every move is a new value)
        pg.mouse.move(cx, kb["y"] + 60 - 10 * (1 + math.sin(n_move * 0.35)))
        pg.wait_for_timeout(16)
    pg.mouse.move(cx, kb["y"] + 60 - 50)                         # the last position: a value the swing never had
    final = pg.evaluate("__fmini.S.p[__fmini.S.pe0]")
    pg.mouse.up(); t_up = time.monotonic(); settled = None
    dev_val = lambda: (lambda r: (r[2] | r[3] << 7) - 8192)(fa(31, [0, pe0]))
    while time.monotonic() - t_up < 6.0:
        if dev_val() == final:
            settled = time.monotonic() - t_up; break
        pg.wait_for_timeout(10)
    sent = pg.evaluate("window.__sent")
    pg.wait_for_timeout(300)
    stays = dev_val() == final                                   # (no older value arrives after it)
    pg.evaluate("window.__delay = 0")
    check(f"slow device, 2 s drag ({n_move} moves): the last value reaches it {'in %.0f ms' % (settled * 1000) if settled is not None else 'NEVER'} after the release (<= 400 ms)", settled is not None and settled <= 0.4)
    check(f"  .. and stays: no older value arrives after it ({sent} writes sent in all; the drag made {n_move} moves)", stays)
    pg.wait_for_timeout(300)

    # patch bytes: OSC > OSC 1 WAVE = PLS N, MOD OSC switch, GLIDE
    pg.click("#tabs button:has-text('OSC')")
    pg.locator(".seg", has_text="OSC 1 WAVE").locator("button", has_text="PLS N").click()
    pg.wait_for_timeout(300)
    g = fa(82, [0, 0])
    check("OSC 1 WAVE: the firmware's patch byte (FMINI_GET) is PLS N", g[2] == 0 and g[3 + 3] == 5)
    check("  .. SLOT is OWN now (200: two 7-bit bytes, protocol 3)", g[3 + 32] + 128 * g[3 + 33] == 200)
    pg.click("#tabs button:has-text('CTRL')")
    pg.locator(".sw", has_text="MOD OSC").click()
    pg.wait_for_timeout(300)
    g = fa(82, [0, 0])
    check("MOD OSC switch: bit 32 of the patch's SW1 on, the rest untouched", g[3 + 12] & 32 and g[3 + 3] == 5)
    pg.locator(".sw", has_text="GLIDE").click()                 # (patch 00 has it on, P_GLIDE not synced: the click turns it off)
    pg.wait_for_timeout(300)
    glide = fa(31, [0, 38]); gl = (glide[2] | glide[3] << 7) - 8192
    g = fa(82, [0, 0])
    check("GLIDE switch off: SW2 bit clear, P_GLIDE 0", not (g[3 + 13] & 1) and gl == 0)
    pg.locator(".sw", has_text="GLIDE").click()
    pg.wait_for_timeout(300)
    glide = fa(31, [0, 38]); gl = (glide[2] | glide[3] << 7) - 8192
    g = fa(82, [0, 0])
    check("GLIDE switch on: SW2 bit set, P_GLIDE = the kept time", (g[3 + 13] & 1) and gl == g[3 + 17] and gl > 0)

    # the keys' octave (extras: carried by FMINI_PUT, read back by FMINI_GET)
    pg.locator(".seg", has_text="OCTAVE").locator("button", has_text="+1").click()
    pg.wait_for_timeout(300)
    g = fa(82, [0, 0])
    check("OCTAVE +1: the device's octave (last byte of FMINI_GET) is 5 = +1 + 4", g[37] == 5 and len(g) == 38 and pg.evaluate("__fmini.S.oct") == 1)
    pg.locator(".seg", has_text="OCTAVE").locator("button", has_text="+1").click()   # (stays: it is a choice, not a toggle)

    # FX enum
    pg.click("#tabs button:has-text('FX')")
    sel = pg.locator(".sel", has_text="TYPE").first
    sel.locator("select").select_option(index=2)
    pg.wait_for_timeout(300)
    r = fa(2, [1, 24]); check("REVERB TYPE select: the global G_RTYPE = 2 (PLATE)", ((r[2] | r[3] << 7) - 8192) == 2)

    # presets
    pg.select_option("#preset", "7")
    pg.wait_for_timeout(700)
    g = fa(82, [0, 0]); f = fa(82, [2, 7])
    check("PRESET 07 from the list: the track plays factory patch 7", g[3:35] == f[3:35] and g[35] == 7)
    check("  .. the page shows it (select + knob values reflect the firmware)", pg.input_value("#preset") == "7")
    pg.click("#next"); pg.wait_for_timeout(700)
    check("next ▶: preset 08", pg.input_value("#preset") == "8" and fa(82, [0, 0])[35] == 8)
    pg.click("#prev"); pg.click("#prev"); pg.wait_for_timeout(900)
    check("◀ ◀: preset 06", pg.input_value("#preset") == "6")

    # the panel follows the device (a change made on the panel of the FM-1)
    fa(31, [0, 1, (50 + 8192) & 127, (50 + 8192) >> 7])
    pg.click("#tabs button:has-text('A.ENV')")
    pg.wait_for_timeout(1700)
    check("a change on the FM-1 shows up on the page (polling)", pg.locator(".knob", has_text="ATTACK").get_attribute("aria-valuenow") == "50")

    # sound file round trip
    with pg.expect_download() as dl:
        pg.click("#save")
    p = Path("/tmp/claude_fmini_sound.json"); dl.value.save_as(p)
    saved = fa(82, [0, 0])[3:35]
    fa(83, [0, 0, *[0] * 32])  # a blank patch on the device
    pg.wait_for_timeout(1500)
    pg.set_input_files("#file", str(p))
    pg.wait_for_timeout(700)
    check("Save / Open sound: the file puts the saved patch back on the device", fa(82, [0, 0])[3:35] == saved)

    # ---- phase 3: the device's presets (the PRESETS tab), A/B, randomize
    pg.on("dialog", lambda d: d.accept())
    pg.click("#tabs button:text-is('PRESETS')")
    pg.wait_for_selector("#resetsel")
    check("PRESETS: the 200 presets are listed for FACTORY RESET", pg.locator("#resetsel option").count() == 200)
    check("  .. slot 150 is a factory patch, not INIT", not pg.locator("#resetsel option").nth(150).inner_text().endswith("INIT"))
    check("  .. no cards, no SAVE / erase / U-bank buttons (a slot is written on the device)", pg.locator(".slot").count() == 0 and pg.locator("button:has-text('Save here')").count() == 0)
    pg.select_option("#preset", "150")
    pg.wait_for_timeout(900)
    check("Load: slot 150 is on the track (the PRESET list)", pg.evaluate("__fmini.S.slot") == 150)
    fa(83, [0, 0, *[5] * 32])
    pg.click("#tabs button:text-is('CTRL')")
    # A/B
    pg.click("#tabs button:text-is('FILT')")
    kn = pg.locator(".knob", has_text="CUTOFF"); base = kn.get_attribute("aria-valuenow")
    pg.select_option("#preset", "3"); pg.wait_for_timeout(800)
    base = pg.evaluate("__fmini.S.p[__fmini.S.pe0]")
    kn.focus()
    for _ in range(4): pg.keyboard.press("ArrowDown")
    pg.wait_for_timeout(400)
    moved = pg.evaluate("__fmini.S.p[__fmini.S.pe0]")
    pg.click("#ab"); pg.wait_for_timeout(500)
    a_val = (lambda r: (r[2] | r[3] << 7) - 8192)(fa(31, [0, pe0]))
    check("A/B: switching to B gives back the sound as the preset was loaded", moved != base and a_val == base)
    pg.click("#ab"); pg.wait_for_timeout(500)
    b_val = (lambda r: (r[2] | r[3] << 7) - 8192)(fa(31, [0, pe0]))
    check("  .. and back to A: the edit is there again", b_val == moved)
    # randomize
    before = fa(82, [0, 0])[3:35]
    pg.fill("#amt", "100") if False else pg.evaluate("document.getElementById('amt').value = 100; document.getElementById('amt').dispatchEvent(new Event('input'))")
    pg.click("#rnd"); pg.wait_for_timeout(600)
    after = fa(82, [0, 0])[3:35]
    check("Randomize: the patch on the device changed and OSC 1 is still on", after != before and after[12] & 1 and all(0 <= x <= 127 for x in after))
    pg.wait_for_timeout(1200)
    check("  .. and the page is usable again (not busy)", not pg.evaluate("document.body.classList.contains('busy')"))

    # ---- phase 4: backup (.bkp) and restore: the device answers YES here (FM_RESTORE_ANSWER: the bridge)
    pg.click("#tabs button:text-is('PRESETS'), #tabs button:text-is('PRESETS')"); pg.wait_for_selector("#resetsel")
    with pg.expect_download(timeout=120000) as dl:
        pg.click("button:has-text('Fazer backup'), button:has-text('Make backup')")
    bk = Path("/tmp/claude_fmini_backup.bkp"); dl.value.save_as(bk)
    check("Backup: a .bkp file", dl.value.suggested_filename.endswith(".bkp"))
    bj = json.loads(bk.read_text())
    check("  .. all 200 presets, each as its record", bj.get("fmini1bkp") == 1 and len(bj["slots"]) == 200 and all(x["rec"][:2] == "01" for x in bj["slots"]))
    orig7 = pg.evaluate("__fmini.S.names[7]")
    orig200 = pg.evaluate("__fmini.S.names.slice(0, 200)")
    mod = json.loads(bk.read_text())
    mod["slots"][7]["rec"] = "01" + mod["slots"][7]["rec"][2:4] + "5a" + mod["slots"][7]["rec"][6:]     # slot 7's name: starts with Z
    mp = Path("/tmp/claude_fmini_mod.bkp"); mp.write_text(json.dumps(mod))
    pg.set_input_files("#file", str(mp))
    pg.wait_for_function("/restaur|restor/i.test(document.getElementById('msg').textContent)", timeout=180000)
    check("Restore: the device's presets are the file's (slot 7 starts with Z)", pg.evaluate("__fmini.S.names[7]").startswith("Z"))
    pg.set_input_files("#file", str(bk))
    pg.wait_for_function("!/Z/.test(__fmini.S.names[7] || '')", timeout=180000)
    check("  .. and the original backup puts the name back", pg.evaluate("__fmini.S.names[7]") == orig7)
    pg.wait_for_timeout(300)

    # ---- FACTORY RESET: slot 7 (changed by the file) one at a time, then all the factory presets
    pg.set_input_files("#file", str(mp))
    pg.wait_for_function("() => !document.body.classList.contains('busy') && __fmini.S.names[7].startsWith('Z')", timeout=180000)
    pg.click("#tabs button:text-is('PRESETS')"); pg.wait_for_selector("#resetsel")
    pg.select_option("#resetsel", "7")
    pg.click("button:text-is('Restore'), button:text-is('Restaurar')")
    pg.wait_for_function("(v) => __fmini.S.names[7] === v", arg=orig7, timeout=60000)
    check("FACTORY RESET one: slot 7 is its factory preset again (the name Z is gone)", pg.evaluate("__fmini.S.names[7]") == orig7)
    pg.set_input_files("#file", str(mp))
    pg.wait_for_function("() => !document.body.classList.contains('busy') && __fmini.S.names[7].startsWith('Z')", timeout=180000)
    pg.click("#tabs button:text-is('PRESETS')"); pg.wait_for_selector("#resetsel")
    pg.click("button:text-is('Restore all factory presets'), button:text-is('Restaurar todos os presets de fábrica')")
    pg.wait_for_function("(v) => !document.body.classList.contains('busy') && __fmini.S.names[7] === v", arg=orig7, timeout=300000)
    check("FACTORY RESET all: the 200 factory presets are back, slot 7 included", pg.evaluate("__fmini.S.names.slice(0, 200)") == orig200)
    check("  .. and the last slot (199) is a factory patch again", pg.evaluate("__fmini.S.names[199]") != "INIT")

    # firmware up to 1.7.1 cut the NAMES reply at 664 bytes: the page says an update is available and still works
    CUT[0] = True
    pg.click("#connect"); pg.wait_for_timeout(300)                # (disconnect)
    pg.click("#connect")
    pg.wait_for_function("!document.body.classList.contains('off')", timeout=15000)
    note = pg.inner_text("#msg"); got = pg.evaluate("__fmini.S.names.filter(Boolean).length")
    check(f"a firmware that cuts NAMES at 664 bytes ({got} of 200 names): the page says an update is available", 60 < got < 100 and ("Update" in note or "Atualiz" in note) and str(got) in note)
    CUT[0] = False
    pg.click("#connect"); pg.wait_for_timeout(300)
    pg.click("#connect")
    pg.wait_for_function("!document.body.classList.contains('off')", timeout=15000)
    check("  .. and the current firmware gives all 200 again, with no notice", pg.evaluate("__fmini.S.names.filter(Boolean).length") == 200 and "Update" not in pg.inner_text("#msg") and "Atualiz" not in pg.inner_text("#msg"))

    if SHOTS:
        SHOTS.mkdir(parents=True, exist_ok=True)
        for i, name in enumerate(["CTRL", "OSC", "MIX", "FILT", "F.ENV", "A.ENV", "FX", "ARP", "PRESETS"]):
            pg.click(f"#tabs button:text-is('{name}')"); pg.wait_for_timeout(200)
            pg.screenshot(path=str(SHOTS / f"ed_{i}_{name.replace('.', '')}.png"))
        pg.set_viewport_size({"width": 390, "height": 800}); pg.click("#tabs button:text-is('OSC')")
        pg.screenshot(path=str(SHOTS / "ed_mobile.png"))
    check("no errors in the page's console", not errs)
    if errs: print(errs)
    br.close()

proc.stdin.close(); proc.wait(timeout=5)
print("editor_fmini test FAILED" if bad else "editor_fmini test passed")
sys.exit(1 if bad else 0)
