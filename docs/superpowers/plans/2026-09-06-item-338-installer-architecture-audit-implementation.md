# ITEM-338 · Release verification / Проверки выпуска

[English](#english) · [Русский](#русский)

HFOC OS `v1.0.0` · `933f7cd739a9437e86638fbfe30a738e22f2e9aa`

## English

### Installer architecture audit

The host check requires the audit sections, writes and recovery boundaries, and safety requirements cited by the installer work. Its success is a documentation contract result.

- `make test-item-338-installer-architecture-audit-host`

[Technical reference](../../hfoc-installer-architecture-audit.md)

## Русский

### Аудит архитектуры установщика

Хостовая проверка требует наличия разделов аудита, границ записи и восстановления, а также требований безопасности для установщика. Её результат подтверждает соблюдение требований к документации.

- `make test-item-338-installer-architecture-audit-host`

[Техническая документация](../../hfoc-installer-architecture-audit.md)
