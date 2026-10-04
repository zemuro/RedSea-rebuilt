"""Note Off секвенсора по спецификации автора и запуск под внешний Clock (docs/REVIEW.md,
«Note Off: спецификация»). Проверка по журналу отправки прошивки: номер такта и порядок сообщений."""
import time

import pytest

from test_seq_clock import send_clocks


def events(tx):
    """[(такт, 'on'|'off'|'cc123', нота, канал)] для нот и All Notes Off."""
    out = []
    for m in tx:
        ch = (m.s & 0x0F) + 1
        if m.kind == "note_on":
            out.append((m.tick, "on", m.d1, ch))
        elif m.kind == "note_off":
            out.append((m.tick, "off", m.d1, ch))
        elif m.kind == "cc" and m.d1 == 123:
            out.append((m.tick, "cc123", None, ch))
    return out


def pattern(rs, notes, steps=4):
    """notes[i] — нота шага i или None (пустой шаг)."""
    rs.clear_steps()
    for i, n in enumerate(notes):
        if n is not None:
            rs.step(i, "1000", note=n)
    rs.set(arm=0, steps=steps, scale=0)


def run_ext(rs, midi, n_clocks):
    rs.txlog_clear()
    midi.start()
    time.sleep(0.05)
    send_clocks(midi, n_clocks, 0.02)
    midi.stop()
    time.sleep(0.2)
    return events(rs.txlog())


@pytest.mark.tid("В1", "P1")
def test_note_sounds_through_empty_steps(rs, midi, rec):
    """Нота звучит через пустые шаги; перед следующей нотой — сначала Off старой, потом On новой."""
    pattern(rs, [60, None, 62, None])
    ev = run_ext(rs, midi, 20)  # шаги на тактах 1, 7, 13, 19
    rec("события [такт, что, нота, канал]", ev)
    assert ev[:3] == [(1, "on", 60, 1), (13, "off", 60, 1), (13, "on", 62, 1)], f"ожидалось On60@1, Off60@13, On62@13; было {ev[:3]}"


@pytest.mark.tid("В1", "P1")
def test_same_note_restarts(rs, midi, rec):
    """Та же нота на следующем активном шаге — Off и сразу On."""
    pattern(rs, [60, 60], steps=2)
    ev = run_ext(rs, midi, 8)
    rec("события", ev)
    assert ev[:3] == [(1, "on", 60, 1), (7, "off", 60, 1), (7, "on", 60, 1)], f"было {ev[:3]}"


@pytest.mark.tid("В1", "P1")
def test_stop_releases_note_and_sends_all_notes_off(rs, midi, rec):
    """Stop: Note Off звучащей ноты и All Notes Off (CC 123)."""
    pattern(rs, [60, None, None, None])
    ev = run_ext(rs, midi, 10)
    rec("события", ev)
    tail = [e[1:] for e in ev[-2:]]
    assert tail == [("off", 60, 1), ("cc123", None, 1)], f"после Stop ожидались Off60 и CC123; было {ev}"


@pytest.mark.tid("В1", "P1")
def test_bypass_releases_note(rs, rec):
    """Вход в BYPASS гасит звучащую ноту до того, как выход заглушится."""
    pattern(rs, [60, None, None, None], steps=4)
    rs.set(bpm=60, bypsel=1)   # BYPASS
    rs.page("main")
    rs.txlog_clear()
    rs.double("tap")
    time.sleep(0.3)            # шаг 1 прозвучал, следующий — через 250 мс при 60 BPM
    rs.click("play")           # вход в BYPASS
    time.sleep(0.2)
    rs.click("play")           # выход
    rs.double("tap")
    ev = events(rs.txlog())
    rec("события", ev)
    first_off = next((i for i, e in enumerate(ev) if e[1] == "off" and e[2] == 60), None)
    assert ev and ev[0][1:3] == ("on", 60), f"нота не прозвучала: {ev}"
    assert first_off == 1, f"при входе в BYPASS Note Off не отправлен: {ev}"


@pytest.mark.tid("В1", "P1")
def test_channel_change_releases_note_on_old_channel(rs, rec):
    """Смена MIDI-канала: Note Off звучащей ноте уходит на прежнем канале."""
    pattern(rs, [60, None, None, None])
    rs.set(bpm=60)
    rs.page("cc", sub=1, sel=0)
    rs.txlog_clear()
    rs.double("tap")
    time.sleep(0.3)
    rs.enc(4)                  # канал 1 -> 2
    rs.double("tap")
    ev = events(rs.txlog())
    rec("события", ev)
    assert (ev[0][1:], ev[1][1:]) == (("on", 60, 1), ("off", 60, 1)), f"ожидались On60 и Off60 на канале 1: {ev}"


@pytest.mark.tid("В6", "P1")
def test_tap_start_under_external_clock_waits_for_beat(rs, midi, rec):
    """Запуск двойным TAP при внешнем Clock: первый шаг — на ближайшей доле (такты 1, 25, 49…)."""
    pattern(rs, [60, 62, 64, 65])
    midi.start()
    time.sleep(0.05)
    send_clocks(midi, 30, 0.02)       # секвенсор запущен Start'ом — остановим его двойным TAP
    rs.double("tap")
    send_clocks(midi, 5, 0.02)
    rs.txlog_clear()
    tap_tick = rs.state()["clk"]["ticks"]
    rs.double("tap")                  # запуск посреди доли
    send_clocks(midi, 60, 0.02)
    midi.stop()
    time.sleep(0.2)
    on = [e for e in events(rs.txlog()) if e[1] == "on"]
    rec("такт нажатия / первая нота", f"{tap_tick} / {on[0] if on else None}")
    assert on, "секвенсор не заиграл"
    assert on[0][2] == 60 and on[0][0] % 24 == 1, f"первая нота {on[0]} — не шаг 1 на доле"
    assert on[0][0] > tap_tick, "первый шаг прозвучал раньше нажатия"
