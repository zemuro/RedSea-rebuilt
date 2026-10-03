// Тестовая обвязка RED SEA. Подключается ко всем исходникам проекта через `-include`
// только в окружении PlatformIO `test` (там же задаётся -DREDSEA_TEST). В обычной сборке
// не участвует, а вставки в REDSEA.ino обёрнуты в #ifdef REDSEA_TEST — сборка автора
// не меняется ни на байт.
//
// Реализация — testfw/redsea_test_impl.h, подключается в конец REDSEA.ino: так ей видны
// `state`, `buttons`, `display` и остальное глобальное состояние прошивки.
#ifndef REDSEA_TEST_H
#define REDSEA_TEST_H

#ifdef REDSEA_TEST

#include <Arduino.h>

void th_setup();                    // USB-консоль, сброс счётчиков
void th_poll();                     // разбор команд консоли (начало loop)
void th_loopBegin();                // начало прохода loop() — длительность прохода
void th_serviceMark();              // вход в processMIDI() — зазоры обслуживания MIDI-входа
void th_tickLate(uint32_t lateUs);  // опоздание внутреннего такта относительно расписания
void th_logRx(uint8_t b);           // сырой байт MIDI-входа
void th_logTx(uint8_t s, uint8_t d1, uint8_t d2);  // сообщение, реально ушедшее на MIDI-выход
void th_scopeRecord(const char* name, uint32_t us);

struct ThScope {
    const char* name;
    uint32_t t0;
    explicit ThScope(const char* n) : name(n), t0(micros()) {}
    ~ThScope() { th_scopeRecord(name, micros() - t0); }
};
#define TH_SCOPE(n) ThScope _th_scope_(n)

// Виртуальные кнопки: пока подмена включена, digitalRead() для пинов кнопок возвращает
// заданный уровень — опрос кнопок прошивки (антидребезг, двойные клики, удержания)
// работает без изменений.
int IRAM_ATTR th_digitalRead(uint8_t pin);
#define digitalRead(p) th_digitalRead(p)

#endif  // REDSEA_TEST
#endif  // REDSEA_TEST_H
