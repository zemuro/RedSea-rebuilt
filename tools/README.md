# tools — испытательный стенд

Автотесты прошивки на живом устройстве: pytest + MIDI + USB-консоль тестовой сборки.
Подробно — [RIG.md](RIG.md): устройство стенда, что переносится из ESPidi как есть, что писать заново,
методика и подводные камни. Устройство — [../docs/device/DEVICE.md](../docs/device/DEVICE.md).

Запуск (Windows): `.\run_tests.ps1 --serial COM5 --midi-in "USB2.0-MIDI" --midi-out "USB2.0-MIDI"`.
