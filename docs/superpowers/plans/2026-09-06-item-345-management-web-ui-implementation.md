# ITEM-345 · Release verification / Проверки выпуска

[English](#english) · [Русский](#русский)

HFOC OS `v1.0.0` · `933f7cd739a9437e86638fbfe30a738e22f2e9aa`

## English

### HTTPS management interface

Host fixtures cover the rendered shell and section mapping. The TLS 1.3 QEMU fixture covers the HTTPS response and authorized section dispatch. This item’s proof measures the software path.

- `make test-appliance-management-web-ui-host`
- `make test-management-plane-qemu`

[Technical reference](../../hfoc-management-web-ui.md)

## Русский

### Интерфейс управления HTTPS

Хостовые сценарии проверяют формирование веб-интерфейса и соответствие адресов разделам. Тест QEMU с TLS 1.3 проверяет ответ HTTPS и обработку разрешённых запросов к разделам. Эти тесты охватывают программную часть.

- `make test-appliance-management-web-ui-host`
- `make test-management-plane-qemu`

[Техническая документация](../../hfoc-management-web-ui.md)
