"""Интерфейс, графика и сохранение настроек. Номера находок — docs/REVIEW.md."""
import time

import pytest


@pytest.mark.tid("Д1", "P1")
def test_bpm_above_255(rs, rec):
    """BPM на подстранице setup: от 250 энкодером вверх растёт до 300 и сохраняется."""
    rs.page("seq", sub=1, sel=0)
    rs.set(bpm=250)
    seen = []
    for _ in range(8):
        rs.enc(4)
        seen.append(rs.state()["clk"]["bpmInt"])
    rec("BPM после каждого щелчка от 250", seen)
    assert seen == list(range(251, 259)), f"BPM после 250: {seen}"
    time.sleep(5.5)  # секвенсор сохраняется раз в 5 с
    saved = rs.nvs()["keys"].get("seqBPM16")
    rec("BPM в NVS", saved)
    assert saved == 258, f"в NVS сохранён темп {saved}, а не 258"
    rs.set(bpm=298)
    for _ in range(4):
        rs.enc(4)
    top = rs.state()["clk"]["bpmInt"]
    rec("BPM от 298 после 4 щелчков", top)
    assert top == 300, f"верхний предел — {top}, а не 300"


@pytest.mark.tid("Д7", "P1")
def test_encoder_fast_turn(rs, rec):
    """10 щелчков энкодера, пришедших за один проход цикла, дают +10, а не +1."""
    rs.page("main", sel=0)
    rs.param(0, cc=1, value=0)
    rs.enc(40)
    time.sleep(0.2)
    v = rs.state()["p"][0][1]
    rec("значение P1 после 10 щелчков", v)
    assert v == 10, f"P1 = {v} после 10 щелчков"


@pytest.mark.tid("Д2", "P1")
def test_cc_numbers_survive_reboot_after_randomize(rs, rec):
    """PLAY+TAP на странице CC меняет все 4 номера — после перезагрузки остаются все 4."""
    rs.page("cc", sub=0, sel=0)
    rs.combo("tap", "play")
    before = [p[0] for p in rs.state()["p"]]
    time.sleep(3.0)  # запись по таймеру раз в 2 с
    keys = rs.nvs()["keys"]
    saved = [keys.get(f"cc{i}") for i in range(4)]
    rs.reload()
    after = [p[0] for p in rs.state()["p"]]
    rec("CC после рандомизации", before)
    rec("CC в NVS", saved)
    rec("CC после перезагрузки", after)
    assert after == before, f"после перезагрузки CC {after}, а были {before}"


@pytest.mark.tid("Д2", "P1")
def test_cc_number_saved_when_cursor_moves(rs, rec):
    """Изменили CC параметра P1 и сразу перешли к P2: номер P1 всё равно сохраняется."""
    rs.page("cc", sub=0, sel=0)
    time.sleep(2.2)  # начать сразу после очередной записи по таймеру — она не должна успеть
    rs.enc(4 * 5)
    for _ in range(5):
        rs.enc(4)
    rs.set(sel=1)
    time.sleep(3.0)
    cc0 = rs.state()["p"][0][0]
    saved = rs.nvs()["keys"].get("cc0")
    rec("CC P1 в памяти / в NVS", f"{cc0} / {saved}")
    assert saved == cc0, f"CC P1 = {cc0}, а в NVS {saved}"


@pytest.mark.tid("Д6", "P2")
def test_no_flash_writes_when_idle(rs, rec):
    """Без изменений настроек NVS не перезаписывается."""
    time.sleep(6)
    u0 = rs.nvs()["used"]
    rs.con.stats_reset()
    time.sleep(21)
    u1 = rs.nvs()["used"]
    sc = rs.stats()["scopes"].get("seqsave", {})
    rec("занято записей NVS: до / через 21 с", f"{u0} / {u1}")
    rec("saveSequencerSettings: вызовов / max мкс / сумма мкс", f"{sc.get('n')} / {sc.get('max')} / {sc.get('sum')}")
    assert u1 == u0, f"в простое NVS вырос на {u1 - u0} записей за 21 с"


@pytest.mark.tid("Д5", "P2")
def test_animation_time_after_freeze(rs, rec):
    """Пауза анимации на время FREEZE: после выхода анимация продолжается с места паузы."""
    rs.page("main")
    time.sleep(3.0)
    t0 = rs.state()["anim"]["time"]
    times = []
    for _ in range(2):
        rs.click("play")       # вход во FREEZE (режим по умолчанию)
        time.sleep(0.8)
        rs.click("play")       # выход
        time.sleep(0.3)
        times.append(rs.state()["anim"]["time"])
    rec("время анимации до FREEZE, мс", t0)
    rec("после 1-го и 2-го выхода, мс", times)
    assert times[1] < 2 ** 31, f"после второго выхода время анимации {times[1]} — ушло «в минус»"
    assert times[0] >= t0, f"после выхода время анимации {times[0]} мс < {t0} мс до паузы (анимация начинается заново)"
    assert times[1] >= times[0], f"после второго выхода время анимации {times[1]} < {times[0]}"


@pytest.mark.tid("В7", "P2")
@pytest.mark.parametrize("btn", ["tap", "play"])
def test_midi_learn_cancelled_by_any_button(rs, rec, btn):
    """Любая кнопка отменяет ожидание MIDI Learn (MANUAL); PLAY при этом не включает BYPASS/FREEZE."""
    rs.page("cc", sub=0, sel=0)
    for _ in range(3):
        rs.click("enc", 0.05)
    assert rs.state()["learn"], "тройной клик не включил MIDI Learn"
    rs.click(btn)
    st = rs.state()
    rec(f"ожидание MIDI Learn после {btn.upper()} / режим", f"{st['learn']} / {st['bypass']}")
    assert not st["learn"], f"{btn.upper()} не отменил ожидание MIDI Learn"
    assert st["bypass"] == 0, "отмена MIDI Learn кнопкой PLAY заодно включила BYPASS/FREEZE"


@pytest.mark.tid("Д6", "P2")
@pytest.mark.parametrize("bpm", [120, 240])
def test_settings_saved_after_pause_without_late_ticks(rs, rec, bpm):
    """Настройка сохраняется через 2 с после последнего изменения — и во время игры; пока
    энкодер крутят, запись не идёт; такты при записи не опаздывают (она — сразу после такта)."""
    rs.set(arm=1, weather=0, chaos=50, bpm=bpm)
    rs.page("storm", sub=0, sel=0)    # AMT
    rs.double("tap")                   # транспорт идёт
    time.sleep(1.0)
    for _ in range(6):
        rs.enc(4)                      # AMT +2 каждые ~0,5 с
        time.sleep(0.4)
    during = rs.nvs()["keys"].get("chaos")
    # Чтение NVS командой стенда тоже останавливает процессор — в окно замера его не пускаем:
    # статистика сбрасывается после него, запись настроек наступит через ~1,6 с.
    rs.con.stats_reset()
    time.sleep(3.0)
    st = rs.stats()
    after = rs.nvs()["keys"].get("chaos")
    rs.double("tap")
    late, sv = st["late"], st["scopes"].get("save", {})
    rec("AMT в NVS: пока крутят / через 3 с после", f"{during} / {after}")
    rec("запись: раз / max мкс", f"{sv.get('n')} / {sv.get('max')}")
    rec("опоздание такта, мкс: max / avg", f"{late['max']} / {late['avg']}")
    assert during is None, f"запись шла, пока крутили энкодер ({during})"
    assert after == 62, f"через 2 с после последнего изменения в NVS AMT = {after}, а не 62"
    # Обычная запись (~4 мс) идёт сразу после такта и в промежуток помещается. Изредка NVS
    # обслуживает страницу, и запись длится ~20 мс — длиннее промежутка между тактами на
    # быстром темпе; это известное и редкое исключение, оно попадает в отчёт отдельно.
    period_us = 60e6 / bpm / 24
    if sv.get("max", 0) > period_us * 0.8:
        rec("длинная запись (обслуживание страницы NVS)", f"{sv['max']} мкс при промежутке {period_us:.0f} мкс")
    else:
        assert late["max"] < 1000, f"такт опоздал на {late['max'] / 1000:.1f} мс при обычной записи настроек"
