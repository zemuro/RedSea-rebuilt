"""Секвенсор и Clock: логика по номерам тактов (журнал отправки прошивки) и тайминг (часы ESP32).

Номера находок — docs/REVIEW.md. Тест описывает правильное поведение: провал = дефект подтверждён.
"""
import time

import pytest

from esptest import thresholds
from esptest.clock import RealtimeClock


def send_clocks(midi, n, gap_s=0.02):
    for _ in range(n):
        midi.clock()
        time.sleep(gap_s)


def notes_on(tx):
    return [m for m in tx if m.kind == "note_on"]


def two_note_pattern(rs):
    """Шаг 1 — нота 60, шаг 2 — нота 62, остальные пустые; движок выключен (нет лишних CC)."""
    rs.clear_steps()
    rs.step(0, "1000", note=60)
    rs.step(1, "1000", note=62)
    rs.set(arm=0, steps=16, scale=0)


@pytest.mark.tid("Д4", "P1")
def test_first_step_after_external_start(rs, midi, rec):
    """После Start первым должен звучать шаг 1 — на первом такте Clock."""
    two_note_pattern(rs)
    rs.txlog_clear()
    midi.start()
    time.sleep(0.05)
    send_clocks(midi, 12, 0.03)
    time.sleep(0.2)
    on = notes_on(rs.txlog())
    rec("ноты [такт, нота]", [(m.tick, m.d1) for m in on])
    assert on, "секвенсор не сыграл ни одной ноты"
    first = on[0]
    assert first.d1 == 60 and first.tick <= 1, (
        f"первой прозвучала нота {first.d1} на такте {first.tick}; ожидалась нота 60 (шаг 1) на такте 1")


@pytest.mark.tid("Д4", "P1")
def test_first_step_after_internal_start(rs, rec):
    """Запуск двойным TAP на внутреннем темпе: первым звучит шаг 1."""
    two_note_pattern(rs)
    rs.set(bpm=120)
    rs.txlog_clear()
    rs.double("tap")
    time.sleep(0.6)
    rs.double("tap")  # стоп
    on = notes_on(rs.txlog())
    rec("ноты [такт, нота]", [(m.tick, m.d1) for m in on])
    assert on, "секвенсор не сыграл ни одной ноты"
    assert on[0].d1 == 60, f"первой прозвучала нота {on[0].d1} (шаг {2 if on[0].d1 == 62 else '?'}), а не 60 (шаг 1)"


@pytest.mark.tid("Д3", "P1")
def test_rain_after_stop_start(rs, midi, rec):
    """RAIN после Stop -> Start продолжает мутировать сразу, а не ждёт, пока счётчик тактов догонит прошлый прогон."""
    rs.set(weather=2, chaos=50, wav=0, arm=1, drip=0, splsh=0, thndr=0)
    for i in range(4):
        rs.param(i, cc=20 + i, value=64)
    midi.start()
    send_clocks(midi, 480, 0.006)  # первый прогон: 480 тактов (5 тактов музыки)
    midi.stop()
    time.sleep(0.2)
    st = rs.state()
    rec("rainNextTick перед вторым Start", st["rainNext"])
    rs.txlog_clear()
    midi.start()
    time.sleep(0.05)
    send_clocks(midi, 48, 0.006)
    time.sleep(0.2)
    cc = [m for m in rs.txlog() if m.kind == "cc" and 20 <= m.d1 <= 23]
    rec("CC от RAIN за 48 тактов после Start", len(cc))
    # WAV = 1/16 (6 тактов): за 48 тактов — 8 раундов по 4 параметра
    assert len(cc) >= 16, f"за 48 тактов после повторного Start RAIN отправил {len(cc)} CC (ожидалось ~32)"


# Время анимации: сразу после включения и «через 10 минут работы». От него зависит цена кадра:
# примерно через 3 минуты аргументы sinf() в анимациях становятся большими, и sinf переходит
# на медленную ветку (кадр MAIN дорожает с ~6 до ~32 мс).
UPTIME = {"boot": 5_000, "10min": 600_000}


@pytest.mark.tid("Г1", "P0")
@pytest.mark.parametrize("uptime", list(UPTIME))
@pytest.mark.parametrize("bpm", [120, 240])
def test_internal_clock_timing(rs, rec, bpm, uptime):
    """Внутренний Clock: опоздание такта < 3 мс и точный темп при работающей графике."""
    rs.set(bpm=bpm, weather=0, chaos=50, arm=1, animtime=UPTIME[uptime])  # FOG: CC на каждом такте
    rs.page("main")
    rs.double("tap")
    time.sleep(1.0)
    rs.con.stats_reset()
    s0 = rs.state()
    time.sleep(thresholds.SHORT_RUN_S)
    s1 = rs.state()
    st = rs.stats()
    rs.double("tap")
    dt = (s1["t"] - s0["t"]) / 1e6
    ticks = s1["clk"]["ticks"] - s0["clk"]["ticks"]
    expect = bpm * 24 / 60 * dt
    tempo = ticks / expect
    late = st["late"]
    rec("тактов / ожидалось", f"{ticks} / {expect:.0f} ({(tempo - 1) * 100:+.2f} %)")
    rec("опоздание такта, мкс: max / avg", f"{late['max']} / {late['avg']}")
    rec("опоздание, гистограмма <0.1/0.5/1/2/5/10/20/≥20 мс", late["b"])
    rec("кадр (updateDisplay), мкс: max", st["scopes"].get("display", {}).get("max"))
    assert abs(tempo - 1) < 0.005, f"темп {bpm} BPM отклоняется на {(tempo - 1) * 100:+.2f} %"
    assert late["max"] < thresholds.MAX_TICK_LATE_US, (
        f"такт опаздывает до {late['max'] / 1000:.1f} мс (допуск {thresholds.MAX_TICK_LATE_US / 1000:.0f} мс)")


@pytest.mark.tid("Г1", "P1")
def test_internal_clock_during_freeze_transition(rs, rec):
    """Анимация перехода BYPASS/FREEZE не должна задерживать такты."""
    rs.set(bpm=120, weather=0, chaos=50, arm=1, animtime=UPTIME["boot"])
    rs.page("main")
    rs.double("tap")
    time.sleep(1.0)
    rs.con.stats_reset()
    for _ in range(6):
        rs.click("play")
        time.sleep(0.4)
    st = rs.stats()
    rec("события > 5 мс [мкс, что, длительность]", rs.con.events()[:12])
    rs.double("tap")
    late = st["late"]
    rec("опоздание такта, мкс: max / avg", f"{late['max']} / {late['avg']}")
    rec("кадр (updateDisplay), мкс: max", st["scopes"].get("display", {}).get("max"))
    assert late["max"] < thresholds.MAX_TICK_LATE_US, f"такт опаздывает до {late['max'] / 1000:.1f} мс"


@pytest.mark.tid("Г2", "P0")
@pytest.mark.parametrize("uptime", list(UPTIME))
def test_external_clock_service_gap(rs, midi, rec, uptime):
    """Внешний Clock: вход MIDI обслуживается не реже раза в 5 мс (иначе такты ждут в буфере UART)."""
    rs.set(weather=0, chaos=50, arm=1, animtime=UPTIME[uptime])
    rs.page("main")
    clk = RealtimeClock(midi, 120)
    clk.start(send_start=True)
    time.sleep(1.0)
    rs.con.stats_reset()
    time.sleep(thresholds.SHORT_RUN_S)
    st = rs.stats()
    clk.stop(send_stop=True)
    gap = st["gap"]
    rec("зазор обслуживания, мкс: max / avg", f"{gap['max']} / {gap['avg']}")
    rec("зазор, гистограмма <0.5/1/2/5/10/20/50/100/≥100 мс", gap["b"])
    late_share = sum(gap["b"][4:]) / max(1, gap["n"])
    rec("доля проходов с зазором ≥ 5 мс", f"{late_share * 100:.1f} %")
    assert gap["max"] < thresholds.MAX_SERVICE_GAP_US, (
        f"вход MIDI не обслуживается до {gap['max'] / 1000:.1f} мс — на столько может опоздать ответ на такт")


@pytest.mark.tid("Г3", "P1")
def test_external_clock_without_start_plus_tap(rs, midi, rec):
    """Clock без Start (DAW в стопе) + запуск секвенсора двойным TAP: темп не удваивается."""
    rs.set(arm=0, bpm=120)
    clk = RealtimeClock(midi, 120)
    clk.start(send_start=False)
    time.sleep(0.5)
    rs.double("tap")
    time.sleep(0.5)
    s0 = rs.state()
    time.sleep(5.0)
    s1 = rs.state()
    clk.stop()
    rs.double("tap")
    rate = (s1["clk"]["ticks"] - s0["clk"]["ticks"]) / ((s1["t"] - s0["t"]) / 1e6)
    rec("тактов в секунду (ожидалось 48)", f"{rate:.1f}")
    assert abs(rate - 48) < 2.5, f"счётчик тактов идёт со скоростью {rate:.1f}/с вместо 48/с"


@pytest.mark.tid("В1", "P1")
def test_sequencer_notes_are_released(rs, rec):
    """Каждая нота секвенсора получает Note Off (к следующему шагу или по Stop)."""
    rs.clear_steps()
    for i, n in enumerate((60, 62, 64, 65)):
        rs.step(i, "1000", note=n)
    rs.set(arm=0, steps=4, bpm=120)
    rs.txlog_clear()
    rs.double("tap")
    time.sleep(1.5)
    rs.double("tap")
    time.sleep(0.2)
    tx = rs.txlog()
    sounding = set()
    for m in tx:
        if m.kind == "note_on":
            sounding.add(m.d1)
        elif m.kind == "note_off":
            sounding.discard(m.d1)
    rec("Note On / Note Off", f"{sum(m.kind == 'note_on' for m in tx)} / {sum(m.kind == 'note_off' for m in tx)}")
    rec("звучат после Stop", sorted(sounding))
    assert not sounding, f"после Stop остались звучать ноты {sorted(sounding)}"


@pytest.mark.tid("В4", "P2")
def test_fog_single_cc_per_tick(rs, midi, rec):
    """FOG: на такте — одно значение на параметр (без случайного выброса перед значением LFO)."""
    rs.set(weather=0, chaos=60, wav=0, arm=1)
    for i in range(4):
        rs.param(i, cc=20 + i, value=64)
    rs.txlog_clear()
    midi.start()
    send_clocks(midi, 24, 0.02)
    midi.stop()
    time.sleep(0.2)
    per = {}
    for m in rs.txlog():
        if m.kind == "cc" and 20 <= m.d1 <= 23:
            per.setdefault((m.tick, m.d1), []).append(m.d2)
    dup = {k: v for k, v in per.items() if len(v) > 1}
    rec("тактов с двумя CC на параметр", len({k[0] for k in dup}))
    rec("пример", list(dup.items())[:3])
    assert not dup, f"на {len({k[0] for k in dup})} тактах из 24 параметр получил два CC подряд"


@pytest.mark.tid("В5", "P2")
def test_fog_lfo_sends_only_changes(rs, midi, rec):
    """FOG: LFO отправляет CC только когда значение изменилось (AMT 0 — значение постоянно)."""
    rs.set(weather=0, chaos=0, wav=2, arm=1)
    for i in range(4):
        rs.param(i, cc=20 + i, value=64)
    rs.txlog_clear()
    midi.start()
    send_clocks(midi, 48, 0.02)
    midi.stop()
    time.sleep(0.2)
    cc = [m for m in rs.txlog() if m.kind == "cc" and 20 <= m.d1 <= 23]
    rec("CC за 48 тактов при постоянном значении", len(cc))
    assert len(cc) <= 4, f"за 48 тактов отправлено {len(cc)} CC с неизменным значением"
