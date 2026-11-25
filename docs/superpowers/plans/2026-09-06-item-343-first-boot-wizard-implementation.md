# ITEM-343 · Release verification / Проверки выпуска

[English](#english) · [Русский](#русский)

HFOC OS `v1.0.0` · `933f7cd739a9437e86638fbfe30a738e22f2e9aa`

## English

### First-boot setup stages

The host proof checks stage order, invalid evidence, exact ready output, and refusal of out-of-order calls. Persistent writes belong to the storage and configuration layers. The proof covers the state machine.

- `make test-installer-first-boot-wizard-host`

[Technical reference](../../hfoc-first-boot-wizard.md)

## Русский

### Этапы настройки при первом запуске

Хостовая проверка охватывает порядок этапов, некорректные данные, точный текст сообщения о готовности и отказы при нарушении порядка вызовов. Запись состояния выполняют подсистемы хранения и конфигурации. Тест проверяет автомат состояний.

- `make test-installer-first-boot-wizard-host`

[Техническая документация](../../hfoc-first-boot-wizard.md)
