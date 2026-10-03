// Реализация тестовой обвязки RED SEA (см. redsea_test.h). Подключается в конец REDSEA.ino
// под #ifdef REDSEA_TEST — видит всё глобальное состояние прошивки.
//
// Протокол USB-консоли: строка `<id> <команда> [аргументы]\n` -> ответ `@<id> {json}\n`.
#ifdef REDSEA_TEST

#undef digitalRead  // внутри обвязки нужен настоящий digitalRead

#include <stdarg.h>
#include <nvs.h>
#include <nvs_flash.h>

// ---------------------------------------------------------------- метрики

struct ThHist {
    uint32_t n = 0, max = 0;
    uint64_t sum = 0;
    uint32_t b[10] = {0};
};
static const uint32_t TH_EDGES_MS[] = {500, 1000, 2000, 5000, 10000, 20000, 50000, 100000};  // мкс
static const uint32_t TH_EDGES_LATE[] = {100, 500, 1000, 2000, 5000, 10000, 20000};          // мкс

static void thHistAdd(ThHist& h, uint32_t v, const uint32_t* edges, int ne) {
    h.n++;
    h.sum += v;
    if (v > h.max) h.max = v;
    int i = 0;
    while (i < ne && v >= edges[i]) i++;
    h.b[i]++;
}

struct ThScopeStat {
    const char* name = nullptr;
    uint32_t n = 0, max = 0;
    uint64_t sum = 0;
};
static ThScopeStat s_scopes[16];

struct ThEv {
    uint32_t t;
    const char* kind;
    uint32_t v;
};
static ThEv s_ev[64];
static uint8_t s_evHead = 0, s_evCount = 0;

static ThHist s_loop, s_gap, s_late;
static uint32_t s_lastLoop = 0, s_lastSvc = 0;

static void thEvPush(const char* kind, uint32_t v) {
    s_ev[s_evHead] = {micros(), kind, v};
    s_evHead = (s_evHead + 1) % 64;
    if (s_evCount < 64) s_evCount++;
}

static void thStatsReset() {
    s_loop = ThHist();
    s_gap = ThHist();
    s_late = ThHist();
    s_lastLoop = 0;
    s_lastSvc = 0;
    s_evHead = s_evCount = 0;
    for (auto& s : s_scopes) s = ThScopeStat();
}

void th_loopBegin() {
    uint32_t now = micros();
    if (s_lastLoop) {
        uint32_t dt = now - s_lastLoop;
        thHistAdd(s_loop, dt, TH_EDGES_MS, 8);
        if (dt > 20000) thEvPush("loop", dt);
    }
    s_lastLoop = now;
}

void th_serviceMark() {
    uint32_t now = micros();
    if (s_lastSvc) {
        uint32_t dt = now - s_lastSvc;
        thHistAdd(s_gap, dt, TH_EDGES_MS, 8);
        if (dt > 20000) thEvPush("gap", dt);
    }
    s_lastSvc = now;
}

void th_tickLate(uint32_t lateUs) {
    thHistAdd(s_late, lateUs, TH_EDGES_LATE, 7);
    if (lateUs > 5000) thEvPush("late", lateUs);
}

void th_scopeRecord(const char* name, uint32_t us) {
    ThScopeStat* slot = nullptr;
    for (auto& s : s_scopes) {
        if (s.name == name || (s.name && strcmp(s.name, name) == 0)) { slot = &s; break; }
        if (!s.name && !slot) slot = &s;
    }
    if (slot) {
        if (!slot->name) slot->name = name;
        slot->n++;
        slot->sum += us;
        if (us > slot->max) slot->max = us;
    }
    if (slot && us > 20000) thEvPush(name, us);
}

// ---------------------------------------------------------------- журналы MIDI

struct ThRx {
    uint32_t t;
    uint8_t b;
};
static ThRx s_rx[512];
static uint16_t s_rxHead = 0, s_rxCount = 0;
static uint32_t s_rxTotal = 0;

void th_logRx(uint8_t b) {
    s_rx[s_rxHead] = {micros(), b};
    s_rxHead = (s_rxHead + 1) % 512;
    if (s_rxCount < 512) s_rxCount++;
    s_rxTotal++;
}

// Исходящие сообщения с номером такта (state.midiTicks) в момент отправки: по нему тесты
// проверяют логику «на каком такте что прозвучало» без шума USB-MIDI и Windows.
struct ThTx {
    uint32_t t, tick;
    uint8_t s, d1, d2;
};
static ThTx s_tx[512];
static uint16_t s_txHead = 0, s_txCount = 0;
static uint32_t s_txTotal = 0;

void th_logTx(uint8_t s, uint8_t d1, uint8_t d2) {
    s_tx[s_txHead] = {micros(), state.midiTicks, s, d1, d2};
    s_txHead = (s_txHead + 1) % 512;
    if (s_txCount < 512) s_txCount++;
    s_txTotal++;
}

// ---------------------------------------------------------------- виртуальные кнопки

static volatile uint8_t s_ovMask = 0;  // бит = номер GPIO, чтение которого подменяем
static volatile uint8_t s_ovLow = 0;   // бит = 1 -> кнопка «нажата» (LOW)

int IRAM_ATTR th_digitalRead(uint8_t pin) {
    if (pin < 8 && ((s_ovMask >> pin) & 1)) return ((s_ovLow >> pin) & 1) ? 0 : 1;
    return digitalRead(pin);
}

static int thPinByName(const char* n) {
    if (!strcmp(n, "play")) return Pins::PLAY;
    if (!strcmp(n, "tap")) return Pins::TAP;
    if (!strcmp(n, "page")) return Pins::PAGE;
    if (!strcmp(n, "enc")) return Pins::ENC_SW;
    return -1;
}

// ---------------------------------------------------------------- вывод

static void jf(String& s, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s += buf;
}

static String s_out;

static void thReply(uint32_t id, const String& json) {
    s_out += '@';
    s_out += String(id);
    s_out += ' ';
    s_out += json;
    s_out += '\n';
}

static void thSendReplies() {
    if (!s_out.length()) return;
    Serial.print(s_out);
    Serial.flush();  // HWCDC держит неполный 64-байтный пакет в FIFO — выталкиваем
    s_out = String();
}

static void thErr(uint32_t id, const char* msg) {
    String s = "{\"err\":\"";
    s += msg;
    s += "\"}";
    thReply(id, s);
}

static void thHistJson(String& s, const char* key, const ThHist& h, int nb) {
    jf(s, "\"%s\":{\"n\":%u,\"max\":%u,\"avg\":%u,\"b\":[", key, (unsigned)h.n, (unsigned)h.max,
       (unsigned)(h.n ? h.sum / h.n : 0));
    for (int i = 0; i < nb; i++) jf(s, "%s%u", i ? "," : "", (unsigned)h.b[i]);
    s += "]}";
}

// ---------------------------------------------------------------- команды

static void cmdState(uint32_t id) {
    String s;
    s.reserve(1600);
    jf(s, "{\"t\":%u,\"ms\":%u,\"page\":%u,\"sub\":[%u,%u,%u,%u],\"sel\":%u,\"heap\":%u,",
       (unsigned)micros(), (unsigned)millis(), (unsigned)state.currentPage, state.subPageMain, state.subPageCC,
       state.subPageStorm, state.subPageSequencer, state.selectedParam, (unsigned)ESP.getFreeHeap());
    jf(s, "\"weather\":%u,\"chaos\":%u,\"wav\":%u,\"arm\":%d,\"bypass\":%u,\"bypsel\":%u,\"ch\":%u,"
          "\"seqdest\":%u,\"gfx\":%d,",
       (unsigned)state.weatherMode, state.chaos, state.waveIntervalIndex, state.randomizerEnabled,
       (unsigned)state.bypassMode, (unsigned)state.selectedBypassMode, state.midiChannel, (unsigned)state.seqDest,
       state.gfxEnabled);
    jf(s, "\"clk\":{\"ticks\":%u,\"midiRun\":%d,\"seqRun\":%d,\"bpmInt\":%u,\"bpmExt\":%.2f},",
       (unsigned)state.midiTicks, state.midiRunning, state.sequencerRunning, (unsigned)state.sequencerBPM,
       (double)state.bpm);
    jf(s, "\"seq\":{\"steps\":%u,\"scale\":%u,\"cc\":%u,\"cursor\":%u,\"play\":%u,\"disp\":%u,\"lastStep\":%u},",
       state.sequencerSteps, state.sequencerScaleIndex, state.sequencerCC, state.sequencerCursor,
       state.sequencerPlayhead, state.sequencerDisplayStep, (unsigned)state.sequencerLastStepTick);
    s += "\"p\":[";
    for (int i = 0; i < NUM_PARAMS; i++)
        jf(s, "%s[%u,%u,%u,%u,%u]", i ? "," : "", state.params[i].cc, state.params[i].value,
           state.params[i].min, state.params[i].max, state.params[i].baseValue);
    s += "],\"frz\":[";
    for (int t = 0; t < SNOW_FREEZE_TARGETS; t++) jf(s, "%s%d", t ? "," : "", getSnowTargetFrozen(t));
    s += "],\"snowFrz\":[";
    for (int t = 0; t < SNOW_FREEZE_TARGETS; t++)
        jf(s, "%s[%d,%u]", t ? "," : "", state.snowFreezeActive[t], (unsigned)state.snowFreezeUntilTick[t]);
    s += "],\"rainNext\":[";
    for (int i = 0; i < NUM_PARAMS; i++) jf(s, "%s%u", i ? "," : "", (unsigned)state.rainNextTick[i]);
    s += "],\"sunRefl\":[";
    for (int i = 0; i < NUM_PARAMS; i++)
        jf(s, "%s[%u,%u]", i ? "," : "", state.sunReflectLevel[i], (unsigned)state.sunReflectNextTick[i]);
    jf(s, "],\"anim\":{\"time\":%u,\"offset\":%u,\"paused\":%d,\"pauseTime\":%u},",
       (unsigned)getAnimTime(), (unsigned)state.animationTimeOffset, state.animationPaused,
       (unsigned)state.animationPauseTime);
    jf(s, "\"save\":{\"mm\":%d,\"cc\":%d,\"storm\":%d,\"glob\":%d},\"learn\":%d,\"enc\":%d}",
       state.needSaveMinMax, state.needSaveCC, state.needSaveStorm, state.needSaveGlobal, state.midiLearnActive,
       (int)encoderTicks);
    thReply(id, s);
}

static void cmdStats(uint32_t id) {
    String s;
    s.reserve(1200);
    s += "{";
    thHistJson(s, "loop", s_loop, 9);
    s += ",";
    thHistJson(s, "gap", s_gap, 9);
    s += ",";
    thHistJson(s, "late", s_late, 8);
    s += ",\"scopes\":{";
    bool first = true;
    for (auto& sc : s_scopes) {
        if (!sc.name) continue;
        jf(s, "%s\"%s\":{\"n\":%u,\"max\":%u,\"sum\":%llu}", first ? "" : ",", sc.name, (unsigned)sc.n,
           (unsigned)sc.max, (unsigned long long)sc.sum);
        first = false;
    }
    s += "}}";
    thReply(id, s);
}

static void cmdEvents(uint32_t id) {
    String s = "{\"ev\":[";
    uint8_t start = (s_evHead + 64 - s_evCount) % 64;
    for (uint8_t i = 0; i < s_evCount; i++) {
        const ThEv& e = s_ev[(start + i) % 64];
        jf(s, "%s[%u,\"%s\",%u]", i ? "," : "", (unsigned)e.t, e.kind, (unsigned)e.v);
    }
    s += "]}";
    s_evHead = s_evCount = 0;
    thReply(id, s);
}

// set <имя> <значение> — прямая установка состояния для подготовки теста (минуя интерфейс).
// Поведение интерфейса проверяется отдельно, виртуальными кнопками и энкодером.
static void cmdSet(uint32_t id, const char* k, const char* vs) {
    if (!k || !vs) return thErr(id, "set <name> <value>");
    int v = atoi(vs);
    if (!strcmp(k, "page")) state.currentPage = (Page)v;
    else if (!strcmp(k, "sub")) {
        switch (state.currentPage) {
            case Page::MAIN: state.subPageMain = v; break;
            case Page::CC: state.subPageCC = v; break;
            case Page::STORM: state.subPageStorm = v; break;
            case Page::SEQUENCER: state.subPageSequencer = v; break;
        }
    }
    else if (!strcmp(k, "sel")) state.selectedParam = v;
    else if (!strcmp(k, "weather")) state.weatherMode = (WeatherMode)v;
    else if (!strcmp(k, "chaos")) state.chaos = v;
    else if (!strcmp(k, "wav")) state.waveIntervalIndex = v;
    else if (!strcmp(k, "arm")) state.randomizerEnabled = v;
    else if (!strcmp(k, "bpm")) state.sequencerBPM = v;
    else if (!strcmp(k, "steps")) state.sequencerSteps = v;
    else if (!strcmp(k, "scale")) state.sequencerScaleIndex = v;
    else if (!strcmp(k, "seqcc")) state.sequencerCC = v;
    else if (!strcmp(k, "cursor")) state.sequencerCursor = v;
    else if (!strcmp(k, "ch")) state.midiChannel = v;
    else if (!strcmp(k, "seqdest")) state.seqDest = (SeqDest)v;
    else if (!strcmp(k, "gfx")) state.gfxEnabled = v;
    else if (!strcmp(k, "bypsel")) state.selectedBypassMode = (BypassMode)v;
    else if (!strcmp(k, "rflct")) state.sunRflct = v;
    else if (!strcmp(k, "arp")) state.sunArp = v;
    else if (!strcmp(k, "dflct")) state.sunDflct = v;
    else if (!strcmp(k, "bias")) state.sunBias = v;
    else if (!strcmp(k, "drip")) state.rainDrip = v;
    else if (!strcmp(k, "wet")) state.rainWet = v;
    else if (!strcmp(k, "splsh")) state.rainSplsh = v;
    else if (!strcmp(k, "thndr")) state.rainThunder = v;
    else if (!strcmp(k, "flake")) state.snowFlake = v;
    else if (!strcmp(k, "rot")) state.snowRotation = v;
    else if (!strcmp(k, "frz")) state.snowFrz = v;
    else if (!strcmp(k, "time")) state.snowTime = v;
    else if (!strcmp(k, "lfotype")) state.lfoType = v;
    else if (!strcmp(k, "shape")) state.lfoShape = v;
    else if (!strcmp(k, "phase")) state.lfoPhase = v;
    else if (!strcmp(k, "glide")) state.lfoGlide = v;
    else if (!strcmp(k, "bypass")) state.bypassMode = (BypassMode)v;  // только вид, без заморозки
    else if (!strcmp(k, "frozen")) setSnowTargetFrozen(v / 10, v % 10);  // frozen <цель*10 + 0|1>
    else if (!strcmp(k, "animtime")) state.animationTimeOffset = millis() - (uint32_t)atol(vs);  // «проработал N мс»
    else if (!strcmp(k, "strike")) state.rainStrikeActive = v;  // вспышка молнии RAIN
    else if (!strcmp(k, "pulse")) state.snowPulseActive = v;    // вспышка снежинки SNOW
    else if (!strcmp(k, "learn")) state.midiLearnActive = v;    // плашка MIDI Learn
    else if (!strcmp(k, "run")) state.sequencerRunning = v;
    else if (!strcmp(k, "midirun")) state.midiRunning = v;
    else return thErr(id, "unknown name");
    state.displayDirty = true;
    thReply(id, "{\"ok\":1}");
}

// param <i> <cc> <value> <min> <max>
static void cmdParam(uint32_t id, char** a) {
    if (!a[0] || !a[4]) return thErr(id, "param <i> <cc> <value> <min> <max>");
    int i = atoi(a[0]);
    if (i < 0 || i >= NUM_PARAMS) return thErr(id, "i 0..3");
    state.params[i].cc = atoi(a[1]);
    state.params[i].value = state.params[i].baseValue = atoi(a[2]);
    state.params[i].min = atoi(a[3]);
    state.params[i].max = atoi(a[4]);
    state.displayDirty = true;
    thReply(id, "{\"ok\":1}");
}

// step <i> <флаги NOTE,CC,DST,RTRG как 4 цифры 0/1> <note> <cc> <dst> <rtrg>
static void cmdStep(uint32_t id, char** a) {
    if (!a[0]) return thErr(id, "step <i> [<flags> <note> <cc> <dst> <rtrg>]");
    int i = atoi(a[0]);
    if (i < 0 || i >= SEQUENCER_STEPS) return thErr(id, "i 0..15");
    SequencerStep& st = state.steps[i];
    if (a[5]) {
        for (int p = 0; p < 4; p++) st.active[p] = a[1][p] == '1';
        st.note = atoi(a[2]);
        st.cc = atoi(a[3]);
        st.wavesIndex = atoi(a[4]);
        st.retrigIndex = atoi(a[5]);
        state.displayDirty = true;
    }
    String s;
    jf(s, "{\"a\":[%d,%d,%d,%d],\"note\":%u,\"cc\":%u,\"dst\":%u,\"rtrg\":%u}", st.active[0], st.active[1],
       st.active[2], st.active[3], st.note, st.cc, st.wavesIndex, st.retrigIndex);
    thReply(id, s);
}

// txlog [<с номера>] | txlog clear — исходящие сообщения по порядковым номерам (с 0 после clear),
// не больше 40 за запрос (ответ ~1,2 КБ). Чтение ничего не удаляет, поэтому запрос можно
// безопасно повторить, если ответ потерялся.
static void cmdTxlog(uint32_t id, const char* a1) {
    if (a1 && !strcmp(a1, "clear")) {
        s_txHead = s_txCount = 0;
        s_txTotal = 0;
        return thReply(id, "{\"ok\":1}");
    }
    uint32_t first = s_txTotal - s_txCount;  // номер самой старой записи в кольце
    uint32_t from = a1 ? (uint32_t)atol(a1) : first;
    if (from < first) from = first;
    uint32_t n = s_txTotal > from ? s_txTotal - from : 0;
    if (n > 40) n = 40;
    String s;
    s.reserve(1400);
    jf(s, "{\"total\":%u,\"from\":%u,\"m\":[", (unsigned)s_txTotal, (unsigned)from);
    for (uint32_t i = 0; i < n; i++) {
        const ThTx& e = s_tx[(s_txHead + 512 - (s_txTotal - from - i)) % 512];
        jf(s, "%s[%u,%u,%u,%u,%u]", i ? "," : "", (unsigned)e.t, (unsigned)e.tick, e.s, e.d1, e.d2);
    }
    s += "]}";
    thReply(id, s);
}

static void cmdRxlog(uint32_t id, const char* a1) {
    if (a1 && !strcmp(a1, "clear")) {
        s_rxHead = s_rxCount = 0;
        s_rxTotal = 0;
        return thReply(id, "{\"ok\":1}");
    }
    uint16_t n = s_rxCount > 200 ? 200 : s_rxCount;
    uint16_t start = (s_rxHead + 512 - n) % 512;
    String s;
    s.reserve(2400);
    jf(s, "{\"total\":%u,\"hex\":\"", (unsigned)s_rxTotal);
    char h[3];
    for (uint16_t i = 0; i < n; i++) {
        snprintf(h, sizeof(h), "%02x", s_rx[(start + i) % 512].b);
        s += h;
    }
    s += "\",\"t\":[";
    for (uint16_t i = 0; i < n; i++) jf(s, "%s%u", i ? "," : "", (unsigned)s_rx[(start + i) % 512].t);
    s += "]}";
    thReply(id, s);
}

// nvs — заполнение раздела NVS (растёт при каждой реальной записи) и сохранённые ключи RED SEA.
static void cmdNvs(uint32_t id) {
    nvs_stats_t st;
    nvs_get_stats(NULL, &st);
    String s;
    s.reserve(800);
    jf(s, "{\"used\":%u,\"free\":%u,\"total\":%u,\"keys\":{", (unsigned)st.used_entries, (unsigned)st.free_entries,
       (unsigned)st.total_entries);
    Preferences p;
    if (p.begin("redsea", true)) {
        static const char* u8keys[] = {"cc0", "cc1", "cc2", "cc3", "min0", "min1", "min2", "min3", "max0",
                                       "max1", "max2", "max3", "chaos", "waveIdx", "weather", "seqSteps",
                                       "seqBPM", "seqCC", "seqScale", "midiCh", "bypassMode", "seqDest"};
        bool first = true;
        for (auto k : u8keys) {
            if (!p.isKey(k)) continue;
            jf(s, "%s\"%s\":%u", first ? "" : ",", k, p.getUChar(k, 0));
            first = false;
        }
        p.end();
    }
    s += "}}";
    thReply(id, s);
}

// ---------------------------------------------------------------- кадры: эталон и профиль

// Прямая работа с шиной экрана в обход задачи передачи (если она есть) — только когда та свободна.
static void thDisplaySync() {
#ifdef REDSEA_DISPLAY_ASYNC
    display.waitIdle();
#endif
}

static uint32_t thFnv(const uint8_t* b, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

// render <t0> <dt> <n> [trans <dir>] — нарисовать n кадров текущей страницы с фиксированным временем
// анимации t0 + k*dt и зерном random() = 1000 + k; ответ — контрольные суммы буфера и время
// updateDisplay() (с передачей по I²C). С `trans` рисуется анимация перехода BYPASS/FREEZE,
// t0 + k*dt — сколько миллисекунд прошло от её начала. По совпадению сумм до и после правки
// проверяется, что картинка не изменилась.
static void cmdRender(uint32_t id, char** a) {
    if (!a[2]) return thErr(id, "render <t0> <dt> <n> [trans <dir>]");
    uint32_t t0 = atol(a[0]), dt = atol(a[1]);
    int n = atoi(a[2]);
    if (n > 40) n = 40;
    bool trans = a[3] && !strcmp(a[3], "trans");
    bool pausedWas = state.animationPaused;
    uint32_t frozenWas = state.frozenAnimTime;
    String s;
    s.reserve(800);
    s = "{\"crc\":[";
    String us = "],\"us\":[";
    for (int k = 0; k < n; k++) {
        uint32_t t = t0 + k * dt;
        randomSeed(1000 + k);
        state.animationPaused = true;
        state.frozenAnimTime = t;
        state.displayDirty = true;
        if (trans) {
            state.bypassTransition = true;
            state.transitionDirection = a[4] && atoi(a[4]);
            state.transitionStart = millis() - t;
        }
        uint32_t c0 = micros();
        updateDisplay();
        uint32_t c1 = micros();
        state.bypassTransition = false;
        jf(s, "%s\"%08x\"", k ? "," : "", (unsigned)thFnv(display.getBuffer(), Display::W * Display::H / 8));
        jf(us, "%s%u", k ? "," : "", (unsigned)(c1 - c0));
    }
    state.animationPaused = pausedWas;
    state.frozenAnimTime = frozenWas;
    state.displayDirty = true;
    s += us;
    s += "]}";
    thReply(id, s);
}

// prof <n> — средняя и максимальная длительность частей кадра текущей страницы (мкс), n повторов.
static void cmdProf(uint32_t id, const char* an) {
    int n = an ? atoi(an) : 20;
    bool bypass = (state.bypassMode == BypassMode::FREEZE);
    uint8_t amt = state.chaos;
    struct Part { const char* name; uint32_t sum, max; } parts[] = {
        {"fill", 0, 0}, {"sea", 0, 0}, {"weather", 0, 0}, {"header", 0, 0}, {"boat", 0, 0},
        {"columns", 0, 0}, {"frame", 0, 0}, {"i2c", 0, 0}};
    auto add = [&](int i, uint32_t d) { parts[i].sum += d; if (d > parts[i].max) parts[i].max = d; };
    for (int k = 0; k < n; k++) {
        uint32_t t = getAnimTime(), c;
        c = micros(); display.fillScreen(bypass ? SSD1306_WHITE : SSD1306_BLACK); add(0, micros() - c);
        c = micros(); drawSeaLines(t, amt, bypass); add(1, micros() - c);
        c = micros(); drawWeather(t, amt, bypass); add(2, micros() - c);
        c = micros(); drawHeader(state.currentPage, state.selectedParam, bypass); add(3, micros() - c);
        c = micros(); drawSailboat(64, 0, t, amt, bypass); add(4, micros() - c);
        c = micros();
        for (uint8_t i = 0; i < NUM_PARAMS; i++)
            drawColumn(i * 32, state.params[i].value, state.selectedParam == i, t, amt, bypass, state.frozen[i]);
        add(5, micros() - c);
        state.displayDirty = true;
        c = micros(); updateDisplay(); add(6, micros() - c);
        thDisplaySync();
        c = micros(); display.display(); add(7, micros() - c);
        delay(5);
    }
    String s = "{";
    for (int i = 0; i < 8; i++)
        jf(s, "%s\"%s\":[%u,%u]", i ? "," : "", parts[i].name, (unsigned)(parts[i].sum / (n ? n : 1)),
           (unsigned)parts[i].max);
    s += "}";
    thReply(id, s);
}

// i2cbg — свободен ли процессор, пока кадр уходит по I²C. Отдельная задача (приоритет выше loop)
// отправляет кадр, а текущая задача тем временем крутит счётчик. Сравниваем с тем же счётчиком
// за то же время без передачи: доля = сколько процессорного времени достаётся остальному коду.
static volatile bool s_bgDone = false;
static void thBgSend(void*) {
    display.display();
    s_bgDone = true;
    vTaskDelete(nullptr);
}

static void thBgSleep(void*) {
    vTaskDelay(pdMS_TO_TICKS(12));
    s_bgDone = true;
    vTaskDelete(nullptr);
}

static void cmdI2cBg(uint32_t id) {
    // Тот же цикл ожидания флага: сначала задача просто спит 12 мс, потом — отправляет кадр.
    thDisplaySync();
    volatile uint32_t idle = 0, busy = 0;
    s_bgDone = false;
    uint32_t t0 = micros();
    xTaskCreate(thBgSleep, "bgsleep", 2048, nullptr, 2, nullptr);
    while (!s_bgDone) idle++;
    uint32_t idleDur = micros() - t0;
    s_bgDone = false;
    t0 = micros();
    xTaskCreate(thBgSend, "bgsend", 4096, nullptr, 2, nullptr);
    while (!s_bgDone) busy++;
    uint32_t dur = micros() - t0;
    String s;
    jf(s, "{\"send_us\":%u,\"idle_per_ms\":%u,\"busy_per_ms\":%u}", (unsigned)dur, (unsigned)(idleDur ? (uint64_t)idle * 1000 / idleDur : 0),
       (unsigned)(dur ? (uint64_t)busy * 1000 / dur : 0));
    thReply(id, s);
}

static void execLine(char* line) {
    char* tok[10] = {nullptr};
    int n = 0;
    char* p = line;
    while (*p && n < 10) {
        while (*p == ' ') *p++ = 0;
        if (!*p) break;
        tok[n++] = p;
        while (*p && *p != ' ') p++;
    }
    if (n < 2) return;
    uint32_t id = (uint32_t)atoi(tok[0]);
    const char* cmd = tok[1];
    char** a = &tok[2];  // аргументы; отсутствующие — nullptr

    if (!strcmp(cmd, "ping")) {
        String s = "{\"t\":" + String((unsigned)micros()) + ",\"rst\":" + String((int)esp_reset_reason()) +
                   ",\"fw\":\"test\",\"app\":\"redsea\",\"build\":\"" __DATE__ " " __TIME__ "\"}";
        return thReply(id, s);
    }
    if (!strcmp(cmd, "state")) return cmdState(id);
    if (!strcmp(cmd, "stats")) {
        if (a[0] && !strcmp(a[0], "reset")) {
            thStatsReset();
            return thReply(id, "{\"ok\":1}");
        }
        return cmdStats(id);
    }
    if (!strcmp(cmd, "events")) return cmdEvents(id);
    if (!strcmp(cmd, "set")) return cmdSet(id, a[0], a[1]);
    if (!strcmp(cmd, "param")) return cmdParam(id, a);
    if (!strcmp(cmd, "step")) return cmdStep(id, a);
    if (!strcmp(cmd, "virt")) {  // virt 1|0 — включить/выключить подмену кнопок
        if (a[0] && atoi(a[0])) {
            s_ovLow = 0;
            s_ovMask = (1 << Pins::PLAY) | (1 << Pins::TAP) | (1 << Pins::PAGE) | (1 << Pins::ENC_SW);
        } else {
            s_ovMask = 0;
        }
        return thReply(id, "{\"ok\":1}");
    }
    if (!strcmp(cmd, "press") || !strcmp(cmd, "release")) {
        int pin = a[0] ? thPinByName(a[0]) : -1;
        if (pin < 0) return thErr(id, "button: play|tap|page|enc");
        if (!strcmp(cmd, "press")) s_ovLow |= (1 << pin);
        else s_ovLow &= ~(1 << pin);
        return thReply(id, "{\"ok\":1}");
    }
    if (!strcmp(cmd, "enc")) {  // enc <переходы>: добавить переходы квадратуры (4 = один щелчок)
        noInterrupts();
        encoderTicks = encoderTicks + (a[0] ? atoi(a[0]) : 4);
        interrupts();
        return thReply(id, "{\"ok\":1}");
    }
    if (!strcmp(cmd, "screen")) {
        String s;
        s.reserve(1040);
        s = "{\"hex\":\"";
        const uint8_t* b = display.getBuffer();
        char h[3];
        for (int i = 0; i < Display::W * Display::H / 8; i++) {
            snprintf(h, sizeof(h), "%02x", b[i]);
            s += h;
        }
        s += "\"}";
        return thReply(id, s);
    }
    if (!strcmp(cmd, "i2c")) {  // i2c <n>: среднее и максимум времени display.display()
        int cnt = a[0] ? atoi(a[0]) : 10;
        uint32_t sum = 0, mx = 0;
        thDisplaySync();
        for (int i = 0; i < cnt; i++) {
            uint32_t t0 = micros();
            display.display();
            uint32_t d = micros() - t0;
            sum += d;
            if (d > mx) mx = d;
        }
        String s;
        jf(s, "{\"n\":%d,\"avg\":%u,\"max\":%u}", cnt, (unsigned)(cnt ? sum / cnt : 0), (unsigned)mx);
        return thReply(id, s);
    }
    if (!strcmp(cmd, "render")) return cmdRender(id, a);
    if (!strcmp(cmd, "prof")) return cmdProf(id, a[0]);
    if (!strcmp(cmd, "i2cbg")) return cmdI2cBg(id);
    if (!strcmp(cmd, "rxlog")) return cmdRxlog(id, a[0]);
    if (!strcmp(cmd, "txlog")) return cmdTxlog(id, a[0]);
    if (!strcmp(cmd, "nvs")) return cmdNvs(id);
    if (!strcmp(cmd, "fresh")) {
        // «Как после стирания и включения», но без перезагрузки: переподключение USB при
        // перезагрузке иногда роняет pyserial на Windows. Состояние — начальные значения
        // State, затем loadSettings() из пустого NVS, как в setup().
        thDisplaySync();
        Preferences p;
        p.begin("redsea", false);
        p.clear();
        p.end();
        state = State();
        lastInternalTickMicros = 0;
        midiRunningStatus = 0;
        midiParseHaveFirstData = false;
        noInterrupts();
        encoderTicks = 0;
        interrupts();
        for (auto& b : buttons) { b.lastStable = HIGH; b.raw = HIGH; b.lastChange = millis(); b.processed = false; }
        while (midi.available()) midi.read();
        loadSettings();
        for (uint8_t i = 0; i < 8; i++) state.frozenBackup[i] = getSnowTargetFrozen(i);
        s_txHead = s_txCount = 0;
        s_txTotal = 0;
        s_rxHead = s_rxCount = 0;
        s_rxTotal = 0;
        thStatsReset();
        return thReply(id, "{\"ok\":1}");
    }
    if (!strcmp(cmd, "reboot") || !strcmp(cmd, "factory")) {
        if (!strcmp(cmd, "factory")) {  // стереть настройки RED SEA и перезагрузиться
            Preferences p;
            p.begin("redsea", false);
            p.clear();
            p.end();
        }
        thReply(id, "{\"ok\":1}");
        thSendReplies();
        delay(150);
        ESP.restart();
        return;
    }
    thErr(id, "unknown command");
}

// ---------------------------------------------------------------- жизненный цикл

void th_setup() {
    Serial.setTxBufferSize(4096);
    Serial.setRxBufferSize(1024);
    Serial.begin(115200);
    Serial.setTxTimeoutMs(30);  // не подвешивать цикл, если порт никто не читает
    thStatsReset();
}

void th_poll() {
    static char line[256];
    static size_t len = 0;
    for (int guard = 0; guard < 256 && Serial.available(); guard++) {
        int c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (len) {
                line[len] = 0;
                execLine(line);
                thSendReplies();
                len = 0;
            }
        } else if (len < sizeof(line) - 1) {
            line[len++] = (char)c;
        } else {
            len = 0;  // переполнение — отбрасываем строку
        }
    }
}

#endif  // REDSEA_TEST
