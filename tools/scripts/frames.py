"""Эталонные кадры экрана: проверка, что оптимизация отрисовки не меняет ни одного пикселя.

    python scripts/frames.py record frames_author.json   # на прошивке автора (окружение test)
    python scripts/frames.py check  frames_author.json   # на изменённой прошивке
    python scripts/frames.py record late.json 600000     # время анимации от 10 минут работы

Прошивка рисует кадры с фиксированным временем анимации и зерном random() (команда `render`)
и возвращает контрольную сумму буфера SSD1306. Сценарии — все страницы и подстраницы, четыре
погоды, несколько AMT, режимы BYPASS/FREEZE, плашки и анимация перехода. Не покрыто: иней
замороженного параметра и вспышки шагов секвенсора — они зависят от millis() и в эталон не годятся.
Заодно печатается время updateDisplay() (с передачей по I²C) по страницам.
"""
from __future__ import annotations

import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from esptest.console import Console, find_serial_port  # noqa: E402
from esptest.redsea import RedSea  # noqa: E402

PAGES = [("main", 0), ("main", 1), ("cc", 0), ("cc", 1), ("storm", 0), ("storm", 1), ("seq", 0), ("seq", 1)]
T0, DT, N = 1234, 977, 8   # время анимации: 8 кадров с шагом ~1 с — разные фазы всех движений


def scenarios():
    for page, sub in PAGES:
        for w in range(4):
            for chaos in (0, 35, 75, 100):
                yield dict(page=page, sub=sub, weather=w, chaos=chaos, bypass=0, gfx=1, sel=(chaos // 30) % 4)
            for byp in (1, 2):
                yield dict(page=page, sub=sub, weather=w, chaos=35, bypass=byp, gfx=1, sel=1)
        yield dict(page=page, sub=sub, weather=1, chaos=50, bypass=0, gfx=0, sel=2)
    for w, extra in ((2, dict(strike=1, thndr=1)), (3, dict(pulse=1))):
        for page, sub in (("main", 0), ("storm", 0)):
            yield dict(page=page, sub=sub, weather=w, chaos=60, bypass=0, gfx=1, sel=0, **extra)
    yield dict(page="cc", sub=0, weather=0, chaos=20, bypass=0, gfx=1, sel=0, learn=1)
    yield dict(page="seq", sub=1, weather=0, chaos=20, bypass=0, gfx=1, sel=2, learn=1)
    for page in ("main", "seq"):
        for d in (0, 1):
            yield dict(page=page, sub=0, weather=1, chaos=40, bypass=2 if d else 0, gfx=1, sel=0, trans=d)


def prepare(rs: RedSea):
    vals = [(10, 0, 127), (50, 20, 100), (90, 0, 127), (127, 60, 127)]
    for i, (v, lo, hi) in enumerate(vals):
        rs.param(i, cc=17 * i + 3, value=v, lo=lo, hi=hi)
    for i in range(16):
        flags = "".join("1" if (i * 7 + p * 3) % 5 < 2 else "0" for p in range(4))
        rs.step(i, flags, note=36 + i * 5, cc=i * 8, dst=i % 5, rtrg=i % 5)
    rs.set(cursor=5, flake=7, rflct=3, arp=2, dflct=40, bias=-12, drip=90, wet=60, splsh=30,
           rot=2, frz=70, time=-20, lfotype=1, shape=40, phase=-30, glide=50, bpm=133, steps=12, seqcc=21)


def run(rs: RedSea, t0=T0):
    out, timing = {}, {}
    for sc in scenarios():
        sc = dict(sc)
        page, sub, trans = sc.pop("page"), sc.pop("sub"), sc.pop("trans", None)
        rs.set(strike=0, pulse=0, learn=0, thndr=0)
        rs.page(page, sub=sub, sel=sc.pop("sel"))
        rs.set(**sc)
        key = f"{page}{sub} " + " ".join(f"{k}={v}" for k, v in sc.items())
        if trans is None:
            r = rs.con.cmd("render", t0, DT, N, timeout=10)
        else:
            key += f" trans={trans}"
            r = rs.con.cmd("render", 0, 10, 15, "trans", trans, timeout=10)
        out[key] = r["crc"]
        timing.setdefault(f"{page}{sub}" + (" trans" if trans is not None else ""), []).extend(r["us"])
    return out, timing


def main():
    mode, path = sys.argv[1], Path(sys.argv[2])
    con = Console(find_serial_port() or "COM5")
    con.open()
    con.handshake()
    con.virt(True)
    rs = RedSea(con)
    rs.fresh()
    prepare(rs)
    t = time.time()
    frames, timing = run(rs, int(sys.argv[3]) if len(sys.argv) > 3 else T0)
    con.virt(False)
    con.close()
    n = sum(len(v) for v in frames.values())
    print(f"{len(frames)} сценариев, {n} кадров за {time.time() - t:.0f} с")
    print("updateDisplay(), мс (среднее / макс):")
    for k, v in timing.items():
        print(f"  {k:10s} {sum(v) / len(v) / 1000:6.1f} / {max(v) / 1000:6.1f}")
    if mode == "record":
        path.write_text(json.dumps(frames, indent=0), "utf-8")
        print("эталон записан:", path)
        return
    ref = json.loads(path.read_text("utf-8"))
    bad = [(k, i) for k, v in ref.items() for i, (a, b) in enumerate(zip(v, frames.get(k, []))) if a != b]
    missing = [k for k in ref if k not in frames]
    if bad or missing:
        print(f"РАЗЛИЧИЯ: {len(bad)} кадров из {sum(len(v) for v in ref.values())}, нет сценариев: {len(missing)}")
        for k, i in bad[:20]:
            print("  ", k, "кадр", i)
        sys.exit(1)
    print("все кадры совпадают с эталоном")


if __name__ == "__main__":
    main()
