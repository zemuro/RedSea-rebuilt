"""Оператор RED SEA для тестов: подготовка состояния, жесты кнопками, журналы.

Состояние для теста готовится прямой установкой (`set`, `param`, `step`) — это быстро и
не зависит от интерфейса. Сам интерфейс (кнопки, энкодер) проверяется виртуальными
кнопками: прошивка опрашивает их тем же кодом, что и настоящие.
"""
from __future__ import annotations

import time
from dataclasses import dataclass

from .console import Console
from .midi import Midi

# Индексы перечислений прошивки
PAGE = dict(main=0, cc=1, storm=2, seq=3)
WEATHER = dict(fog=0, sun=1, rain=2, snow=3)
BYPASS = dict(off=0, bypass=1, freeze=2)


@dataclass(frozen=True)
class Tx:
    """Сообщение, ушедшее с MIDI-выхода устройства (журнал прошивки)."""
    t_us: int
    tick: int        # state.midiTicks в момент отправки
    s: int
    d1: int
    d2: int

    @property
    def kind(self):
        k = self.s & 0xF0
        if k == 0x90 and self.d2 == 0:
            return "note_off"
        return {0x80: "note_off", 0x90: "note_on", 0xB0: "cc"}.get(k, f"{self.s:02x}")


class RedSea:
    def __init__(self, con: Console, midi: Midi | None = None):
        self.con, self.midi = con, midi

    # ------------------------------------------------------------ подготовка
    def fresh(self):
        """Настройки «из коробки»: NVS RED SEA стёрт, состояние — как после включения.
        Без перезагрузки (команда `fresh` обвязки): переподключение USB иногда роняет pyserial."""
        for b in ("play", "tap", "page", "enc"):
            self.con.release(b)
        time.sleep(0.1)
        self.con.cmd("fresh", timeout=8)
        time.sleep(0.2)

    def set(self, **kw):
        for k, v in kw.items():
            self.con.cmd("set", k, int(v))

    def param(self, i, cc, value=0, lo=0, hi=127):
        self.con.cmd("param", i, cc, value, lo, hi)

    def step(self, i, flags="0000", note=60, cc=0, dst=2, rtrg=0):
        """flags — активность колонок NOTE, CC, DST, RTRG строкой из 0/1."""
        return self.con.cmd("step", i, flags, note, cc, dst, rtrg)

    def clear_steps(self):
        for i in range(16):
            self.step(i)

    def page(self, name, sub=0, sel=0):
        self.set(page=PAGE[name])
        self.set(sub=sub, sel=sel)

    def state(self) -> dict:
        return self.con.state()

    # ------------------------------------------------------------ кнопки
    def click(self, btn, hold_s=0.09):
        self.con.press(btn)
        time.sleep(hold_s)
        self.con.release(btn)
        time.sleep(0.08)

    def double(self, btn):
        """Двойной клик: два коротких нажатия в окне 300 мс."""
        self.con.press(btn)
        time.sleep(0.06)
        self.con.release(btn)
        time.sleep(0.06)
        self.con.press(btn)
        time.sleep(0.06)
        self.con.release(btn)
        time.sleep(0.1)

    def combo(self, hold, btn):
        self.con.press(hold)
        time.sleep(0.08)
        self.con.press(btn)
        time.sleep(0.08)
        self.con.release(btn)
        time.sleep(0.08)
        self.con.release(hold)
        time.sleep(0.1)

    def enc(self, transitions):
        """Переходы квадратуры энкодера (4 = один щелчок), добавленные разом."""
        self.con.cmd("enc", transitions)
        time.sleep(0.1)

    # ------------------------------------------------------------ журналы и метрики
    def txlog(self) -> list[Tx]:
        """Все исходящие сообщения с последней очистки (журнал прошивки, до 512 последних)."""
        out, frm = [], 0
        while True:
            r = self.con.cmd("txlog", frm)
            out += [Tx(*m) for m in r["m"]]
            frm = r["from"] + len(r["m"])
            if frm >= r["total"] or not r["m"]:
                return out

    def txlog_clear(self):
        self.con.cmd("txlog", "clear")

    def nvs(self) -> dict:
        return self.con.cmd("nvs")

    def stats(self) -> dict:
        return self.con.stats()
