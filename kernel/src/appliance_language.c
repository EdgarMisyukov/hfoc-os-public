/*
 * ITEM-509 increment 1: the one string catalog of the appliance interface.
 *
 * This translation unit is pure data plus a bounded lookup, a strict UTF-8
 * reader and the code-point inventory derived from the catalog.  It allocates
 * nothing, reads no state and prints nothing; increment 1 wires no surface, so
 * every existing rendering and every pinned literal stays as it was.  The
 * entries below are the vocabulary the following increments consume: the panel
 * section names (increment 4), the language control of the console and the
 * panel (increments 2 and 4) and the first help words (increments 5 and 6).
 * The English values are the current interface wording, kept verbatim.
 */
#include "appliance_language.h"

struct appliance_language_entry {
    const char *key;
    const char *en;
    const char *ru;
};

static const struct appliance_language_entry appliance_language_catalog[] = {
    /* The thirteen panel sections, exactly as the navigation names them today. */
    {"panel.section.dashboard", "Dashboard", "Обзор"},
    {"panel.section.network", "Network", "Сеть"},
    {"panel.section.services", "Services", "Службы"},
    {"panel.section.storage", "Storage", "Хранилище"},
    {"panel.section.system", "System", "Система"},
    {"panel.section.security", "Security", "Безопасность"},
    {"panel.section.diagnostics", "Diagnostics", "Диагностика"},
    {"panel.section.updates", "Updates", "Обновления"},
    {"panel.section.recovery", "Recovery", "Восстановление"},
    {"panel.section.routing", "Routing", "Маршрутизация"},
    {"panel.section.account", "Account", "Учётная запись"},
    {"panel.section.certificates", "Certificates", "Сертификаты"},
    /*
     * ITEM-509 increment 10: the Help section became the thirteenth section in
     * increment 8; this is its key.  The navigation label, the page heading and
     * the static index nav all read this one entry, so the section cannot be
     * named three different ways.
     */
    {"panel.section.help", "Help", "Справка"},
    /* The panel's language control: both the active and the saved reading. */
    {"panel.language.label", "Language", "Язык"},
    {"panel.language.effective", "Active language", "Действующий язык"},
    {"panel.language.saved", "Saved language", "Сохранённый язык"},
    /*
     * ITEM-513: the two nav buttons of the per-browser switch.  The labels are
     * the language codes themselves and read identically in both catalogues:
     * they name a language, they are not translated prose.  The values the
     * buttons submit are the catalog's codes, written by the renderer through
     * `appliance_language_code()`, so a label and a cookie value cannot drift.
     */
    {"panel.language.button_en", "EN", "EN"},
    {"panel.language.button_ru", "RU", "RU"},
    /*
     * ITEM-509 increment 10: the panel footer.  It is the one string every
     * panel document carries, so it lives in the catalog like the navigation
     * labels; the English value is the exact text that stood in the renderer
     * before this increment.
     */
    {"panel.footer", "HFOC Appliance management panel",
     "Панель управления HFOC Appliance"},
    /*
     * ITEM-509 increment 11 (509.7a-2a): the read-side pages of the panel.
     * These entries are the panel's prose, landed here before the wiring so the
     * next step (509.7a-2b) replaces literals with lookups without touching
     * wording.  Fragments that surround a dynamic value or markup are separate
     * keys: the renderer prints the markup and the escaped value, the catalog
     * supplies the words.  State values (AVAILABLE, UNAVAILABLE, DEGRADED),
     * field names, commands and routes stay English.
     */
    {"panel.view.hostname", "Hostname", "Имя узла"},
    {"panel.view.mac", "Management MAC", "MAC управления"},
    {"panel.view.service", "Service", "Служба"},
    {"panel.view.detail", "Detail", "Подробности"},
    {"panel.view.recent_log", "Recent log", "Последние записи"},
    {"panel.state.owner", "Owner: ", "Владелец: "},
    {"panel.state.degraded",
     " publishes a mutation-only surface, so live read state stays DEGRADED.",
     " публикует только поверхность изменений, поэтому живое состояние чтения "
     "остаётся DEGRADED."},
    {"panel.state.read_side", " publishes the read side of this section.",
     " публикует сторону чтения этого раздела."},
    {"panel.state.unavailable",
     "No management API owner is bound to this section, so no read side is "
     "published for it. The panel publishes it as UNAVAILABLE instead of "
     "hiding it.",
     "К этому разделу не привязан владелец management API, поэтому сторона "
     "чтения для него не публикуется. Панель публикует его как UNAVAILABLE, а "
     "не скрывает."},
    {"panel.state.security_no_read",
     "No read side is published for this section; the certificate import form "
     "below is its mutation surface.",
     "Для этого раздела сторона чтения не публикуется; форма импорта "
     "сертификата ниже — его поверхность изменений."},
    {"panel.state.help_source_prefix",
     "Rendered from the built-in help catalog, the same text the console prints "
     "for ",
     "Собрано из встроенного каталога справки — тот же текст, который консоль "
     "печатает для "},
    {"panel.state.help_source_suffix", "; no management API owner is involved.",
     "; владелец management API здесь не участвует."},
    {"panel.read.saved", "Saved ", "Сохранено "},
    {"panel.read.active", "Active ", "Действует "},
    {"panel.read.not_invented", "; it does not invent one.",
     "; она ничего не выдумывает."},
    {"panel.read.saved_applies", "Saved applies at the next boot.",
     "Сохранённое применяется при следующей загрузке."},
    {"panel.routes.title", "Applied routes", "Применённые маршруты"},
    {"panel.routes.none_published",
     "No route table is published, so this page cannot list one; it does not "
     "invent an empty table.",
     "Таблица маршрутов не опубликована, поэтому страница не может её "
     "перечислить; она не изобретает пустую таблицу."},
    {"panel.routes.none_applied",
     "No route is applied, so every public name is refused.",
     "Ни один маршрут не применён, поэтому любое публичное имя отвергается."},
    {"panel.routes.one", " route applied", " маршрут применён"},
    {"panel.routes.many", " routes applied", " маршрутов применено"},
    {"panel.routes.truncated", " of them listed here",
     " из них перечислено здесь"},
    {"panel.network.title", "Network configuration", "Настройка сети"},
    {"panel.network.none_published",
     "No network owner is published, so this page cannot show the active or "
     "the saved configuration",
     "Владелец сети не опубликован, поэтому страница не может показать "
     "действующую или сохранённую конфигурацию"},
    {"panel.system.title", "System configuration", "Настройка системы"},
    {"panel.system.none_published",
     "No system owner is published, so this page cannot show the active or the "
     "saved hostname",
     "Владелец системы не опубликован, поэтому страница не может показать "
     "действующее или сохранённое имя узла"},
    {"panel.system.language_active", "Language active ", "Действующий язык "},
    {"panel.account.none_published",
     "No account owner is published, so this page cannot show the login",
     "Владелец учётной записи не опубликован, поэтому страница не может "
     "показать логин"},
    {"panel.account.login", "Login ", "Логин "},
    {"panel.account.change_prefix",
     "A change applies immediately: it signs out every session, including the "
     "one that confirms it. The password is set on the machine console with ",
     "Изменение применяется сразу: оно завершает все сеансы, включая тот, "
     "которым его подтвердили. Пароль задаётся в консоли машины командой "},
    {"panel.account.change_suffix", " if this page is unreachable.",
     ", если эта страница недоступна."},
    {"panel.acme.title", "ACME", "ACME"},
    {"panel.acme.none_published",
     "No ACME state is published, so this page cannot show it",
     "Состояние ACME не опубликовано, поэтому страница не может его показать"},
    {"panel.acme.state", "state ", "состояние "},
    {"panel.acme.account", " account ", " аккаунт "},
    {"panel.acme.renewal", " renewal ", " продление "},
    {"panel.acme.expires", " expires ", " истекает "},
    {"panel.acme.attempts", " attempts ", " попытки "},
    {"panel.acme.order", " order ", " заказ "},
    {"panel.acme.names_note", ". Names are on the Certificates page; ",
     ". Имена — на странице сертификатов; "},
    {"panel.acme.control_link", "enable, disable or request issuance",
     "включение, выключение и запуск выпуска"},
    {"panel.acme.control_tail", " on the ACME control page, two-phase.",
     " на странице управления ACME, двухфазно."},
    {"panel.certificates.none_published",
     "No certificate owner is published, so this page cannot show the declared "
     "names",
     "Владелец сертификатов не опубликован, поэтому страница не может показать "
     "объявленные имена"},
    {"panel.certificates.active_names", "Active names=", "Действующие имена="},
    {"panel.certificates.default", " default ", " по умолчанию "},
    {"panel.certificates.source", " source ", " источник "},
    {"panel.certificates.applies", ", applies=", ", применяется="},
    {"panel.help.not_available", "Help is not available", "Справка недоступна"},
    {"panel.help.command_prefix", "The console command ", "Команда консоли "},
    {"panel.help.command_suffix",
     " reads the same catalog and refuses the same way.",
     " читает тот же каталог и отказывает так же."},
    {"panel.help.index_prefix",
     "Help index, from the console's own catalog: ",
     "Оглавление справки из каталога самой консоли: "},
    /*
     * ITEM-509 increment 25 (509.7d-3c-1): the two Help prose fragments the
     * panel still printed as English literals.  The `index_intro` sentence is
     * split around the fixed `<code>help</code>` markup so each language can
     * order the sentence its own way; the leading and trailing spaces are part
     * of the values because they join the decimal count before and the markup
     * after.
     */
    {"panel.help.all_topics", "All help topics", "Все разделы справки"},
    {"panel.help.index_intro",
     " topics from the built-in catalog; each one opens the same text the "
     "console's ",
     " разделов встроенного каталога; каждый открывает тот же текст, что и "
     "команда "},
    {"panel.help.index_intro_tail", " command prints.", " в консоли."},
    /* ITEM-509 increment 12 (509.7a-3): the prose of the forms, the
       two-phase confirmations, the operator certificate import, the network
       mode, the certificate import and the route form.  The English values
       are the literals the renderers print today, byte for byte, so an
       English document does not move; nothing reads these keys yet. */
    {"panel.form.stage", "Stage", "Подготовить"},
    {"panel.form.confirm", "Confirm", "Подтвердить"},
    {"panel.form.cancel", "Cancel", "Отмена"},
    {"panel.form.token", "token", "token"},
    /*
     * ITEM-647: one label for every visible form control.  The confirmation
     * code is one short term on every surface, with the same one-line note
     * saying when it appears; the other keys name the input they belong to,
     * so a field is never identified only by its position or placeholder.
     */
    {"panel.form.confirm_code", "Confirmation code", "Код подтверждения"},
    {"panel.form.confirm_hint", "Appears after the change is staged.",
     "Появляется после подготовки изменения."},
    {"panel.form.hostname", "Hostname", "Имя устройства"},
    {"panel.form.language", "Panel language", "Язык панели"},
    {"panel.form.current_password", "Current password", "Текущий пароль"},
    {"panel.form.new_login", "New login", "Новый логин"},
    {"panel.form.new_password", "New password", "Новый пароль"},
    {"panel.form.repeat_password", "Repeat new password",
     "Повтор нового пароля"},
    {"panel.form.certificate_name", "Certificate name", "Имя сертификата"},
    {"panel.form.certificate_pem", "Certificate PEM", "Сертификат PEM"},
    {"panel.form.chain_pem", "Chain PEM (optional)",
     "Цепочка PEM (необязательно)"},
    {"panel.form.key_pem", "Private key PEM", "Закрытый ключ PEM"},
    {"panel.form.default_certificate", "Default certificate",
     "Сертификат по умолчанию"},
    {"panel.form.route_name", "Route name", "Имя маршрута"},
    {"panel.form.backend_address", "Server IPv4", "IPv4 сервера"},
    {"panel.form.backend_port", "Server port", "Порт сервера"},
    {"panel.form.tls_to_server", "TLS to server (off for HTTP)",
     "TLS до сервера (off для HTTP)"},
    {"panel.form.route_add", "Add route", "Добавить маршрут"},
    {"panel.form.route_remove", "Remove route", "Удалить маршрут"},
    {"panel.form.route_name_remove", "Route name to remove",
     "Имя маршрута для удаления"},
    {"panel.form.stage_add", "Stage addition", "Подготовить добавление"},
    {"panel.form.stage_remove", "Stage removal", "Подготовить удаление"},
    {"panel.confirm.title", "Two-phase confirmation",
     "Двухфазное подтверждение"},
    {"panel.confirm.none", "No mutation confirmation is pending.",
     "Ожидающего подтверждения изменения нет."},
    {"panel.confirm.mutation", "Mutation: ", "Изменение: "},
    {"panel.confirm.token", "Confirmation token: ", "Токен подтверждения: "},
    {"panel.confirm.single_use", "single-use", "одноразовый"},
    {"panel.confirm.reusable", "reusable", "многоразовый"},
    {"panel.confirm.expires", "Expires in: ", "Истекает через: "},
    {"panel.confirm.window_suffix",
     " ticks. Confirm by repeating the mutation request with this exact token"
     " inside the window; a second confirmation is refused.",
     " тиков. Подтвердите, повторив запрос изменения с этим самым токеном"
     " внутри окна; второе подтверждение отклоняется."},
    {"panel.confirm.pending", "Pending", "Ожидает"},
    {"panel.confirm.token_inline", " token ", " токен "},
    {"panel.confirm.ms_left", " ms left; single-use.",
     " мс осталось; одноразовый."},
    {"panel.security.title", "Import operator certificate",
     "Импорт сертификата оператора"},
    {"panel.security.pending_title", "Pending import", "Ожидает импорта"},
    {"panel.security.token_inline", ", token ", ", токен "},
    {"panel.security.expires_inline", ", expires in ", ", истекает через "},
    {"panel.security.ticks_single_use",
     " ticks; single-use. The certificate is served after the next reboot.",
     " тиков; одноразовый. Сертификат начнёт обслуживаться после следующей"
     " перезагрузки."},
    {"panel.security.placeholder_certificate", "certificate PEM",
     "сертификат PEM"},
    {"panel.security.placeholder_chain", "chain PEM (optional)",
     "цепочка PEM (необязательно)"},
    {"panel.security.placeholder_key", "key PEM", "ключ PEM"},
    {"panel.network.mode_static", "Static", "Статический"},
    /*
     * ITEM-515: the Network form names its controls.  The four fields were
     * drawn bare, so nothing on the page said that `address` and `prefix` are
     * two fields or that the prefix is the number alone; the labels say both.
     */
    {"panel.network.label_mode", "Mode", "Режим"},
    {"panel.network.label_address", "Address", "Адрес"},
    {"panel.network.label_prefix", "Prefix (bits, no slash)",
     "Префикс (биты, без «/»)"},
    {"panel.network.label_gateway", "Gateway", "Шлюз"},
    {"panel.network.label_dns", "DNS", "DNS"},
    {"panel.certificates.import", "Import", "Импорт"},
    {"panel.routing.form_title", "Stage a route", "Подготовить маршрут"},
    /* The licenses surface: the prose around the machine state words
       (AVAILABLE / DEGRADED / UNAVAILABLE) and the LICENSE_* refusal codes,
       which stay English like every other machine value. */
    {"panel.licenses.title", "About and licenses", "О системе и лицензиях"},
    {"panel.licenses.appliance", "Appliance ", "Аппарат "},
    {"panel.licenses.payload", "Payload ", "Полезная нагрузка "},
    {"panel.licenses.shipped", ", shipped notices ",
     ", уведомлений поставлено "},
    {"panel.licenses.of", " of ", " из "},
    {"panel.licenses.declared", " declared.", " объявлено."},
    {"panel.licenses.none_published",
     "No declared dependency inventory is published for this build, so the"
     " panel publishes no license list at all: it never invents one.",
     "Для этой сборки объявленный перечень зависимостей не опубликован,"
     " поэтому панель вовсе не публикует список лицензий: она его не"
     " выдумывает."},
    {"panel.licenses.reason", "Reason: ", "Причина: "},
    {"panel.licenses.column_component", "Component", "Компонент"},
    {"panel.licenses.column_version", "Version", "Версия"},
    {"panel.licenses.column_license", "License", "Лицензия"},
    {"panel.licenses.column_notice", "Notice", "Уведомление"},
    {"panel.licenses.election", "mbedTLS election ", "Выбор mbedTLS "},
    {"panel.licenses.election_from", " from ", " из "},
    /* The ACME control surface: the prose around the machine state word and
       the record fields (requested_at/source/applies stay machine). */
    {"panel.acme_control.title", "ACME control", "Управление ACME"},
    {"panel.acme_control.none_published",
     "No ACME control record is published, so this page cannot show the"
     " setting",
     "Запись управления ACME не опубликована, поэтому эта страница не может"
     " показать настройку"},
    {"panel.acme_control.none_suffix",
     "; it does not invent one.  Nothing is enabled and no issuance is"
     " requested.",
     "; она ничего не выдумывает.  Ничего не включено и выпуск не запрошен."},
    {"panel.acme_control.requested", "requested ", "запрошено "},
    {"panel.acme_control.at", " at ", " в "},
    {"panel.acme_control.source", " source ", " источник "},
    {"panel.acme_control.two_phase_note",
     ". Each action is two-phase: the first form stages it and answers with a"
     " single-use token; the confirmation form commits it.",
     ". Каждое действие двухфазное: первая форма подготавливает его и отвечает"
     " одноразовым токеном; форма подтверждения выполняет его."},
    {"panel.acme_control.enable", "Enable", "Включить"},
    {"panel.acme_control.disable", "Disable", "Отключить"},
    {"panel.acme_control.request", "Request issuance", "Запросить выпуск"},
    /* The sign-in surface: the prose around the machine state words
       (THROTTLED / SIGNED IN / SIGNED OUT) and the fixed technical tokens
       (credentials, Authorization: Bearer, HttpOnly, Secure,
       SameSite=Strict, /api/v1), which the renderer keeps in <code>. */
    {"panel.login.title", "Management sign-in", "Вход в управление"},
    {"panel.login.login", "Login ", "Логин "},
    {"panel.login.password", "Password ", "Пароль "},
    {"panel.login.sign_in", "Sign in", "Войти"},
    {"panel.login.sign_out", "Sign out", "Выйти"},
    {"panel.login.set_account_prefix",
     "Set the account on the machine console with ",
     "Задайте учётную запись в консоли машины командой "},
    {"panel.login.set_account_suffix",
     ", then sign in here with the same login and password.",
     ", затем войдите здесь с тем же логином и паролем."},
    {"panel.login.requirements_prefix",
     "The panel requires an administrator session. Sign in above with the"
     " login and password set on the local console, or present the issued"
     " token in the ",
     "Панель требует сеанса администратора. Войдите выше с логином и паролем,"
     " заданными в локальной консоли, или предъявите выданный токен в "},
    {"panel.login.requirements_middle",
     " request header. A sign-in answers one ",
     " заголовке запроса. Один вход выдаёт "},
    {"panel.login.cookie_separator", ", ", ", "},
    {"panel.login.cookie_suffix",
     " session cookie with a short lifetime. The panel loads no browser"
     " script and no external asset.",
     " сессионный cookie с коротким сроком жизни. Панель не загружает ни"
     " браузерных сценариев, ни внешних ресурсов."},
    {"panel.login.session", "Session: ", "Сессия: "},
    {"panel.login.session_prefix", "session ", "сессия "},
    {"panel.login.principal_prefix", ", principal ", ", субъект "},
    {"panel.login.throttle", "Throttle: ", "Ограничение: "},
    {"panel.login.throttle_suffix",
     " ticks remaining before another sign-in is attempted.",
     " тиков осталось до следующей попытки входа."},
    {"panel.login.api_discovery", "Management API discovery: ",
     "Обнаружение Management API: "},
    /* The console command that shows and sets the one system setting. */
    {"console.language.usage", "USAGE: language en|ru",
     "ИСПОЛЬЗОВАНИЕ: language en|ru"},
    {"console.language.current", "LANGUAGE", "ЯЗЫК"},
    {"console.language.saved", "LANGUAGE SAVED", "ЯЗЫК СОХРАНЁН"},
    {"console.language.applies", "APPLIES TO CONSOLE AND PANEL",
     "Действует для консоли и панели — сразу"},
    {"console.language.refused", "UNKNOWN LANGUAGE: use en or ru",
     "НЕИЗВЕСТНЫЙ ЯЗЫК: используйте en или ru"},
    {"console.language.unavailable", "LANGUAGE IS NOT AVAILABLE",
     "ЯЗЫК НЕДОСТУПЕН"},
    /*
     * ITEM-516: the console's management-gate vocabulary and the warning the
     * operator must read before enabling the setting.  The warning is the
     * contract text of the item: with the setting the machine no longer
     * requires physical presence, the panel answers over the network right
     * after boot and is protected only by the password and the failure
     * lockout, and - audit finding R01 - the management service listens on a
     * wildcard address, so an appliance exposed to the Internet exposes the
     * panel to the whole network.  The machine records beside the prose stay
     * English.
     */
    {"console.gate.usage",
     "USAGE: gate | gate open | gate open confirm | gate close",
     "ИСПОЛЬЗОВАНИЕ: gate | gate open | gate open confirm | gate close"},
    {"console.gate.current", "MANAGEMENT GATE", "ГЕЙТ УПРАВЛЕНИЯ"},
    {"console.gate.value_open", "OPEN AFTER BOOT", "ОТКРЫТ ПОСЛЕ ЗАГРУЗКИ"},
    {"console.gate.value_closed", "CLOSED AFTER BOOT", "ЗАКРЫТ ПОСЛЕ ЗАГРУЗКИ"},
    {"console.gate.warning_short_open",
     "the panel answers over the network without a local login",
     "панель отвечает по сети без локального входа"},
    {"console.gate.warning_short_closed",
     "the panel waits for a local login on this machine",
     "панель ждёт локального входа на этой машине"},
    {"console.gate.refused", "UNKNOWN GATE COMMAND",
     "НЕИЗВЕСТНАЯ КОМАНДА ГЕЙТА"},
    {"console.gate.local_secret_required",
     "REFUSED: run login first; opening the gate requires the administrator "
     "secret proven on this machine",
     "ОТКАЗ: сначала выполните login; открытие гейта требует доказанного "
     "на этой машине секрета администратора"},
    {"console.gate.unconfirmed",
     "REFUSED: no gate open command was requested",
     "ОТКАЗ: команда gate open не запрашивалась"},
    {"console.gate.unavailable", "MANAGEMENT GATE IS NOT AVAILABLE",
     "ГЕЙТ УПРАВЛЕНИЯ НЕДОСТУПЕН"},
    {"console.gate.write_failed", "MANAGEMENT GATE WAS NOT SAVED",
     "ГЕЙТ УПРАВЛЕНИЯ НЕ СОХРАНЁН"},
    {"console.gate.saved_open",
     "SAVED: the panel will answer after every boot without a local login",
     "СОХРАНЕНО: панель будет отвечать после каждой загрузки без локального "
     "входа"},
    {"console.gate.saved_closed",
     "SAVED: the panel waits for a local login again",
     "СОХРАНЕНО: панель снова ждёт локального входа"},
    {"console.gate.warning_title",
     "WARNING: you are about to let this machine answer the panel over the "
     "network right after boot.",
     "ВНИМАНИЕ: машина начнёт отвечать панелью по сети сразу после загрузки."},
    {"console.gate.warning_no_presence",
     "The machine will NO LONGER REQUIRE PHYSICAL PRESENCE: the panel answers "
     "over the network without anyone logging in locally.",
     "Машина БОЛЬШЕ НЕ ТРЕБУЕТ ФИЗИЧЕСКОГО ПРИСУТСТВИЯ: панель отвечает по "
     "сети без локального входа."},
    {"console.gate.warning_password_only",
     "The panel is then protected only by the administrator password.",
     "Панель защищена тогда только паролем администратора."},
    {"console.gate.warning_lockout",
     "After five failed sign-ins the account is locked for a bounded cooldown; "
     "that lockout is the second and last protection.",
     "После пяти неудачных входов учётная запись блокируется на ограниченное "
     "время; эта блокировка — вторая и последняя защита."},
    {"console.gate.warning_wildcard",
     "Audit finding R01: the management service listens on a wildcard address, "
     "so its access scope does not limit who can reach it.",
     "Находка аудита R01: служба управления слушает wildcard-адрес, поэтому "
     "объявленная область доступа не ограничивает, кто может до неё дойти."},
    {"console.gate.warning_internet",
     "On an appliance exposed to the Internet this setting means the panel is "
     "open to the whole network.",
     "На аппарате, выставленном в Интернет, эта настройка означает открытую "
     "панель для всей сети."},
    {"console.gate.warning_confirm",
     "Type gate open confirm on the next line to enable it, or any other "
     "command to cancel.",
     "Наберите gate open confirm на следующей строке, чтобы включить, или "
     "любую другую команду, чтобы отменить."},
    /*
     * ITEM-516: the management-gate page.  The warning is the item's contract
     * text and is rendered on the page before the operator can reach the
     * setting; the console prints the same sentences through its own keys.  A
     * page whose catalog cannot answer renders the named refusal instead of a
     * silent English document.
     */
    {"panel.gate.title", "Management gate", "Гейт управления"},
    {"panel.gate.none_published",
     "The system record was not read, so the setting is unknown",
     "Системная запись не прочитана, поэтому настройка неизвестна"},
    {"panel.gate.source", "source=", "источник="},
    {"panel.gate.refused", "refused=", "отказ="},
    {"panel.gate.warning_title", "What this setting means",
     "Что означает эта настройка"},
    {"panel.gate.warning_no_presence",
     "With the gate open the machine no longer requires physical presence: "
     "the panel answers over the network right after boot, without a local "
     "login.",
     "С открытым гейтом машина больше не требует физического присутствия: "
     "панель отвечает по сети сразу после загрузки, без локального входа."},
    {"panel.gate.warning_password_only",
     "The panel is then protected only by the administrator password.",
     "Панель защищена тогда только паролем администратора."},
    {"panel.gate.warning_lockout",
     "After five failed sign-ins the account is locked for a bounded cooldown; "
     "that lockout is the second and last protection.",
     "После пяти неудачных входов учётная запись блокируется на ограниченное "
     "время; эта блокировка — вторая и последняя защита."},
    {"panel.gate.warning_wildcard",
     "Audit finding R01: the management service listens on a wildcard address, "
     "so its declared scope does not limit who can reach it.",
     "Находка аудита R01: служба управления слушает wildcard-адрес, поэтому "
     "объявленная область доступа не ограничивает, кто может до неё дойти."},
    {"panel.gate.warning_internet",
     "On an appliance exposed to the Internet this setting means the panel is "
     "open to the whole network.",
     "На аппарате, выставленном в Интернет, эта настройка означает открытую "
     "панель для всей сети."},
    {"panel.gate.local_only",
     "Enabling is local-only: it is done at the machine's console, where the "
     "administrator secret is proven.  This page can only close the gate.",
     "Включение возможно только локально: на консоли машины, где доказан "
     "секрет администратора.  Эта страница может только закрыть гейт."},
    {"panel.gate.close", "Close the gate", "Закрыть гейт"},
    {"panel.gate.security_link", "Back to the Security section",
     "Вернуться в раздел «Безопасность»"},
    {"panel.gate.control_link", "Management gate",
     "Гейт управления"},
    {"panel.gate.control_tail",
     ": whether the panel answers after boot without a local login.",
     ": отвечает ли панель после загрузки без локального входа."},
    /*
     * ITEM-509 increment 20: the credential conversation the console's
     * `credentials`/`login` commands and the installer's account screen share.
     * The questions keep their trailing space (the operator types after them);
     * the refusals keep theirs (they are printed before the repeated
     * question), exactly as the module wrote them.
     */
    {"credential.prompt.current", "Current password: ",
     "Текущий пароль: "},
    {"credential.prompt.new_login", "New login: ", "Новый логин: "},
    {"credential.prompt.new_password", "New password: ", "Новый пароль: "},
    {"credential.prompt.confirm", "Repeat new password: ",
     "Повторите новый пароль: "},
    {"credential.prompt.login", "Login: ", "Логин: "},
    {"credential.prompt.password", "Password: ", "Пароль: "},
    {"credential.refusal.too_long",
     "That line is longer than the policy allows. ",
     "Строка длиннее, чем допускает политика. "},
    {"credential.refusal.current_empty",
     "The current password cannot be empty. ",
     "Текущий пароль не может быть пустым. "},
    {"credential.refusal.login_policy",
     "Login must be 1-32 characters: letters, digits, dot, underscore or "
     "hyphen. ",
     "Логин: 1-32 символа, буквы, цифры, точка, подчёркивание или дефис. "},
    {"credential.refusal.password_policy",
     "Password must be 12-128 printable characters, not all spaces and "
     "not the login. ",
     "Пароль: 12-128 печатных символов, не только пробелы и не логин. "},
    {"credential.refusal.weak_password",
     "That password is in the weak list. ",
     "Этот пароль есть в списке слабых. "},
    {"credential.refusal.mismatch", "The passwords do not match. ",
     "Пароли не совпадают. "},
    {"credential.refusal.password_empty", "The password cannot be empty. ",
     "Пароль не может быть пустым. "},
    {"credential.cancelled", "Credential entry cancelled.",
     "Ввод учётных данных отменён."},
    /*
     * The menus (increment 9): the local menu the machine paints and the boot
     * console screen.  The English values are the exact renderings that stood
     * here before this increment, so every pinned line and every QEMU marker
     * stays where it was; the Russian boot-console labels keep the fifteen-cell
     * column of the English ones, so the screen does not turn into a ragged
     * list.
     */
    {"menu.local.title", "LOCAL MENU", "ЛОКАЛЬНОЕ МЕНЮ"},
    {"menu.local.hint", "UP/DOWN select, ENTER run, Q leave",
     "ВВЕРХ/ВНИЗ — выбор, ENTER — запуск, Q — выход"},
    {"menu.local.status", "Status", "Статус"},
    {"menu.local.network", "Network", "Сеть"},
    {"menu.local.diagnostics", "Diagnostics", "Диагностика"},
    {"menu.local.reboot", "Reboot", "Перезагрузка"},
    {"menu.local.shutdown", "Shut down", "Выключение"},
    {"menu.local.recovery", "Recovery", "Восстановление"},
    {"menu.local.unknown", "UNKNOWN", "НЕИЗВЕСТНО"},
    {"menu.bootconsole.hostname", "Hostname:      ", "Имя узла:      "},
    {"menu.bootconsole.cpu", "CPU:           ", "ЦП:            "},
    {"menu.bootconsole.memory", "Memory:        ", "Память:        "},
    {"menu.bootconsole.storage", "Storage:       ", "Хранилище:     "},
    {"menu.bootconsole.health", "Health:        ", "Состояние:     "},
    {"menu.bootconsole.network", "Network:       ", "Сеть:          "},
    {"menu.bootconsole.services", "Services:      ", "Службы:        "},
    {"menu.bootconsole.management", "Management:    ", "Управление:    "},
    {"menu.bootconsole.local_admin", "Local administration:",
     "Локальное администрирование:"},
    {"menu.bootconsole.entry.status", "[1] Status", "[1] Статус"},
    {"menu.bootconsole.entry.network", "[2] Network", "[2] Сеть"},
    {"menu.bootconsole.entry.diagnostics", "[3] Diagnostics",
     "[3] Диагностика"},
    {"menu.bootconsole.entry.reboot", "[4] Reboot", "[4] Перезагрузка"},
    {"menu.bootconsole.entry.shutdown", "[5] Shutdown", "[5] Выключение"},
    {"menu.bootconsole.entry.recovery", "[6] Recovery",
     "[6] Восстановление"},
    /*
     * ITEM-509 increment 21: the console's own offer and availability lines
     * around the local menu.  The values carry their newline because the
     * console writes each one as a single atomic line; the machine records
     * (`[LOCAL-MENU] offered ...`) stay English and are not here.
     */
    {"console.menu.render_failed", "LOCAL MENU: RENDER FAILED\n",
     "ЛОКАЛЬНОЕ МЕНЮ: ОШИБКА ОТРИСОВКИ\n"},
    {"console.menu.no_local_input",
     "LOCAL INPUT UNAVAILABLE: THE MENU IS SHOWN FOR READING\n",
     "ЛОКАЛЬНЫЙ ВВОД НЕДОСТУПЕН: МЕНЮ ПОКАЗАНО ДЛЯ ЧТЕНИЯ\n"},
    {"console.menu.offer_idle",
     "PRESS ENTER FOR THE LOCAL MENU (N: NETWORK SETUP - SET THE ADDRESS "
     "THERE)\n",
     "ENTER — ЛОКАЛЬНОЕ МЕНЮ (N: НАСТРОЙКА СЕТИ — ЗАДАТЬ АДРЕС)\n"},
    {"console.menu.offered_reading",
     "THE LOCAL MENU IS OFFERED BELOW FOR READING\n",
     "ЛОКАЛЬНОЕ МЕНЮ ПРЕДЛОЖЕНО НИЖЕ ДЛЯ ЧТЕНИЯ\n"},
    /*
     * ITEM-509 increment 22: the console's field labels.  A label is written
     * right before its machine value, and some carry the leading space that
     * separates them from the previous field, so the values here are the
     * literals byte for byte, trailing space included.  The values themselves
     * (`ON`/`OFF`, numbers, `REFUSED: ...`) stay English machine text.
     */
    {"console.label.uptime", "UPTIME: ", "ВРЕМЯ РАБОТЫ: "},
    {"console.label.ticks", "TICKS: ", "ТИКИ: "},
    {"console.label.if", "IF: ", "ФЛАГ ПРЕРЫВАНИЙ: "},
    {"console.label.pic_master_mask", " PIC MASTER MASK: ",
     " МАСКА PIC MASTER: "},
    {"console.label.pic_slave_mask", " SLAVE MASK: ", " МАСКА PIC SLAVE: "},
    {"console.label.timer_irq", " TIMER IRQ: ", " IRQ ТАЙМЕРА: "},
    {"console.label.irq_keyboard", " KEYBOARD IRQ: ", " IRQ КЛАВИАТУРЫ: "},
    {"console.label.keyboard_irq", "KEYBOARD IRQ: ", "IRQ КЛАВИАТУРЫ: "},
    {"console.label.last_scancode", " LAST SCANCODE: ",
     " ПОСЛЕДНИЙ СКАНКОД: "},
    {"console.label.usable_memory", "USABLE MEMORY: ", "ДОСТУПНАЯ ПАМЯТЬ: "},
    {"console.label.power", "POWER: ", "ПИТАНИЕ: "},
    {"console.label.unknown_command", "UNKNOWN COMMAND: ",
     "НЕИЗВЕСТНАЯ КОМАНДА: "},
    {"console.label.management", "MANAGEMENT: ", "УПРАВЛЕНИЕ: "},
    /*
     * ITEM-509 increment 23: the console's remaining operator prose - whole
     * sentences rather than labels.  The two sentences that carry their own
     * newline are read through the local-menu helper (its fallback ends the
     * line), the two suffixes through the label helper (its fallback does not);
     * both helpers already exist, so no third name is introduced.  The journal
     * records, the service lines, the diagnostic triggers and the machine
     * values around this prose stay English on purpose.
     */
    {"console.prose.bootlog_unavailable", "BOOTLOG: UNAVAILABLE\n",
     "ЖУРНАЛ ЗАГРУЗКИ: НЕДОСТУПЕН\n"},
    {"console.prose.unknown_command_hint", ". TYPE HELP.\n",
     ". НАБЕРИТЕ HELP.\n"},
    {"console.prose.ptr_range_suffix", "> sets it)", "> задаёт его)"},
    {"console.prose.arm_range_suffix", "> sets the mouse depth)",
     "> задаёт глубину мыши)"},
    /*
     * ITEM-509 increment 24: the last two console sentences that were still
     * English literals.  The leading space of the power sentence belongs to the
     * literal because it follows the action name on the same line, and the
     * `[ADMIN-ENROLLMENT] ` marker before the account sentence stays English
     * machine text.  Both English values are byte for byte the old output.
     */
    {"console.prose.power_refused",
     " REFUSED: this platform declares no mechanism\n",
     " ОТКАЗ: эта платформа не объявляет механизм\n"},
    {"console.prose.credentials_busy",
     "a credentials conversation is already open; press ENTER on an empty line "
     "to cancel it\n",
     "разговор об учётной записи уже открыт; нажмите ENTER на пустой строке, "
     "чтобы отменить его\n"},
    /*
     * ITEM-610: the immediate answer to ENTER on a credential line.  The
     * verification is synchronous and can take a while, so the operator sees
     * that the machine took the line before it starts, without a fake
     * percentage or a success that has not happened yet.
     */
    {"console.prose.credentials_checking", "Checking...\n",
     "Проверка...\n"},
    /*
     * ITEM-618: the scrollback viewer's one hint line.  The keys are the
     * machine's own arrow/home/end events, so the operator never has to guess
     * a PageUp the keyboard does not deliver.
     */
    {"console.prose.scroll_hint",
     " UP/DOWN: SCROLL  HOME/END: ENDS  Q/ESC: BACK\n",
     " ВВЕРХ/ВНИЗ: ПРОКРУТКА  HOME/END: КРАЯ  Q/ESC: НАЗАД\n"},
    {"console.prose.scroll_empty", "SCROLLBACK: EMPTY\n",
     "ИСТОРИЯ ЭКРАНА: ПУСТО\n"},
    /* The first help words, filled with topics in increments 5 and 6. */
    {"help.title", "Help", "Справка"},
    {"help.topics", "TOPICS", "ТЕМЫ"},
    {"help.usage", "TYPE help <topic>, help all",
     "НАБЕРИТЕ help <ТЕМА>, help all"},
    {"help.topic_missing", "NO SUCH HELP TOPIC", "РАЗДЕЛ СПРАВКИ НЕ НАЙДЕН"},
    /* The help topics, one title/summary/body/related set each (increment 7). */
    {"help.topic.install.title", "Installation and disk choice",
     "Установка и выбор диска"},
    {"help.topic.install.summary",
     "install to a whole disk, keep another system, and what is refused",
     "установка на целый диск, сохранение другой системы и отказы"},
    {"help.topic.install.body",
     "The medium carries two entries. The operator entry installs the disk\n"
     "the person at the machine names; the unattended entry writes only the\n"
     "disk its declaration names and answers TARGET_NOT_FOUND on any other\n"
     "machine, because nobody is standing there to name one.\n"
     "Nothing is written before the installer has shown the disk, the\n"
     "partitions it found, what will be removed and what will be preserved.\n"
     "A whole-disk clean install starts only after the explicit\n"
     "confirmation; the path alongside an existing system keeps that system\n"
     "and adds HFOC to the boot menu. A machine that cannot install says\n"
     "why on its own screen (FIRMWARE_UNSUPPORTED, NO_DISK,\n"
     "REPORT_UNAVAILABLE) and waits five minutes before the ordinary boot\n"
     "continues.",
     "Носитель несёт две записи. Запись оператора ставит систему на диск,\n"
     "который назвал человек за машиной; безлюдная запись пишет только диск,\n"
     "названный её объявлением, и на любой другой машине отвечает\n"
     "TARGET_NOT_FOUND, потому что назвать диск там некому.\n"
     "Ничего не пишется, пока установщик не показал диск, найденные разделы,\n"
     "что будет удалено и что сохранено. Установка на целый диск начинается\n"
     "только после явного подтверждения; путь рядом с существующей системой\n"
     "сохраняет её и добавляет HFOC в меню загрузки. Машина, которая не\n"
     "может установиться, называет причину на отдельном экране\n"
     "(FIRMWARE_UNSUPPORTED, NO_DISK, REPORT_UNAVAILABLE) и ждёт пять минут,\n"
     "после чего обычная загрузка продолжается."},
    {"help.topic.install.related",
     "RELATED: PANEL STORAGE, RECOVERY; CONSOLE APPLIANCE, BOOTLOG",
     "СВЯЗАНО: РАЗДЕЛЫ ПАНЕЛИ ХРАНИЛИЩЕ, ВОССТАНОВЛЕНИЕ; КОНСОЛЬ APPLIANCE, BOOTLOG"},
    {"help.topic.network.title", "Network and addresses",
     "Сеть и адреса"},
    {"help.topic.network.summary",
     "the address the appliance uses, and when a change is applied",
     "адрес аппарата и момент применения изменения"},
    {"help.topic.network.body",
     "DHCP is the default: the management interface asks for a lease, and\n"
     "the Network section shows what is active and what is saved, with the\n"
     "source of each reading. A static address, prefix, gateway and DNS\n"
     "server are staged on the same page and written to the durable record\n"
     "only when the single-use confirmation token comes back; the saved\n"
     "configuration applies at the next boot, and the page says so.\n"
     "Management is bound to the management interface only. When no lease\n"
     "arrives the network is published as unavailable or degraded with the\n"
     "reason, never as an address the appliance does not have.",
     "По умолчанию используется DHCP: интерфейс управления запрашивает\n"
     "адрес, а раздел «Сеть» показывает, что действует и что сохранено, с\n"
     "источником каждого чтения. Постоянный адрес, префикс, шлюз и сервер\n"
     "DNS задаются на той же странице и попадают в долговременную запись\n"
     "только после возврата одноразового токена подтверждения; сохранённая\n"
     "настройка применяется при следующей загрузке, и страница это говорит.\n"
     "Управление привязано только к интерфейсу управления. Если адрес не\n"
     "получен, сеть публикуется как недоступная или деградировавшая с\n"
     "причиной, а не как адрес, которого у аппарата нет."},
    {"help.topic.network.related",
     "RELATED: PANEL NETWORK, ROUTING; CONSOLE APPLIANCE",
     "СВЯЗАНО: РАЗДЕЛЫ ПАНЕЛИ СЕТЬ, МАРШРУТИЗАЦИЯ; КОНСОЛЬ APPLIANCE"},
    {"help.topic.storage.title", "Disks and state",
     "Диски и состояние"},
    {"help.topic.storage.summary",
     "what the state volume holds and what destroys it",
     "что хранит том состояния и что его уничтожает"},
    {"help.topic.storage.body",
     "The Storage section has no read side bound yet, and it says that\n"
     "instead of showing an invented table: it renders the appliance's\n"
     "general view and names that no storage owner has published one. The\n"
     "state volume the appliance really keeps carries the system record\n"
     "(hostname and language), the network configuration, the route table,\n"
     "the certificate and secret stores, the recovery metadata and the\n"
     "diagnostic log.\n"
     "A clean installation erases those state stores and reads them back\n"
     "empty before it initializes them, and a run that did not erase cannot\n"
     "reach its verified result. Reinstalling the appliance therefore\n"
     "destroys that state; the installer shows the disk and what will be\n"
     "removed, and writes nothing without the explicit confirmation. A\n"
     "record that cannot be read is published as UNAVAILABLE, never as a\n"
     "default that looks real.",
     "У раздела «Хранилище» ещё нет привязанной стороны чтения, и он так и\n"
     "говорит вместо выдуманной таблицы: он показывает общий вид аппарата и\n"
     "называет, что владелец хранилища ничего не опубликовал. Том\n"
     "состояния, который аппарат действительно ведёт, несёт системную\n"
     "запись (имя узла и язык), настройку сети, таблицу маршрутов, хранилища\n"
     "сертификатов и секретов, метаданные восстановления и журнал\n"
     "диагностики.\n"
     "Чистая установка стирает эти хранилища состояния и читает их обратно\n"
     "пустыми до того, как их инициализирует, а прогон, который не стёр, не\n"
     "может достичь проверенного результата. Поэтому переустановка аппарата\n"
     "уничтожает это состояние; установщик показывает диск и то, что будет\n"
     "удалено, и ничего не пишет без явного подтверждения. Запись, которую\n"
     "нельзя прочитать, публикуется как НЕДОСТУПНА, а не как правдоподобное\n"
     "умолчание."},
    {"help.topic.storage.related",
     "RELATED: PANEL STORAGE, RECOVERY; CONSOLE APPLIANCE",
     "СВЯЗАНО: РАЗДЕЛЫ ПАНЕЛИ ХРАНИЛИЩЕ, ВОССТАНОВЛЕНИЕ; КОНСОЛЬ APPLIANCE"},
    {"help.topic.account.title", "The administrator account",
     "Учётная запись администратора"},
    {"help.topic.account.summary",
     "the one account, its password rule and how it is changed",
     "единственная учётная запись, правило пароля и смена"},
    {"help.topic.account.body",
     "The appliance has one administrator account. It is set when the\n"
     "machine is first set up, and it is the account the panel's sign-in\n"
     "form checks. The login is at most 32 characters; the password is from\n"
     "12 to 128 characters and may not repeat the login. Only a derived\n"
     "verifier is stored, never the password itself, and five failed\n"
     "attempts lock the account for a bounded cooldown.\n"
     "Change it on the machine with the console word credentials, or from\n"
     "the Account section of the panel: current password, new login, new\n"
     "password twice, then the single-use confirmation token. A confirmed\n"
     "change applies at once and signs out every session, including the one\n"
     "that confirmed it, and the page says so.\n"
     "CREDENTIALS RESET followed by CREDENTIALS RESET CONFIRM is the\n"
     "break-glass path and exists only on the machine console; it rewrites\n"
     "the account to the factory placeholder and closes every session.",
     "У аппарата одна учётная запись администратора. Она задаётся при первой\n"
     "настройке машины, и именно её проверяет форма входа в панели. Логин —\n"
     "не длиннее 32 знаков; пароль — от 12 до 128 знаков и не может\n"
     "повторять логин. Хранится только производное проверочное значение, а\n"
     "не сам пароль, и пять неудачных попыток запирают учётную запись на\n"
     "ограниченное время.\n"
     "Сменить её можно на машине словом credentials или в разделе\n"
     "«Учётная запись» панели: текущий пароль, новый логин, новый пароль\n"
     "дважды, затем одноразовый токен подтверждения. Подтверждённая смена\n"
     "действует сразу и завершает все сеансы, включая тот, которым её\n"
     "подтвердили, и страница это говорит.\n"
     "CREDENTIALS RESET, а затем CREDENTIALS RESET CONFIRM — аварийный\n"
     "путь, он есть только на консоли машины; он возвращает учётную запись к\n"
     "заводской заготовке и закрывает все сеансы."},
    {"help.topic.account.related",
     "RELATED: PANEL ACCOUNT; CONSOLE CREDENTIALS, LOGIN",
     "СВЯЗАНО: РАЗДЕЛ ПАНЕЛИ УЧЁТНАЯ ЗАПИСЬ; КОНСОЛЬ CREDENTIALS, LOGIN"},
    {"help.topic.certificates.title", "Certificates and ACME",
     "Сертификаты и ACME"},
    {"help.topic.certificates.summary",
     "your own certificate, automatic issuance and the names they serve",
     "свой сертификат, автоматический выпуск и имена, которым они служат"},
    {"help.topic.certificates.body",
     "The Security section imports the operator's own certificate: a PEM\n"
     "certificate, an optional chain and its key. The import is staged and\n"
     "written only after the single-use token; a pair that does not match,\n"
     "a certificate outside its validity window and an oversized bundle are\n"
     "refused by name (BAD_PAIR, BAD_CLOCK, TOO_LARGE) and nothing is\n"
     "stored. The Certificates section lists the names the appliance holds\n"
     "and never prints a key or a certificate body.\n"
     "The ACME page shows whether automatic issuance is enabled and what\n"
     "the last order did; enabling, disabling and requesting an order are\n"
     "staged there the same way. An order starts only when the appliance\n"
     "has a usable clock, a ready challenge listener and its account\n"
     "identity; until then the request stays pending and the panel names\n"
     "the reason instead of showing a success it did not have.",
     "Раздел «Безопасность» принимает собственный сертификат оператора:\n"
     "сертификат PEM, необязательную цепочку и ключ. Импорт проходит\n"
     "подготовку и пишется только после одноразового токена; несовпадающая\n"
     "пара, сертификат вне срока действия и слишком большой набор\n"
     "отклоняются по имени (BAD_PAIR, BAD_CLOCK, TOO_LARGE), и ничего не\n"
     "сохраняется. Раздел «Сертификаты» перечисляет имена, которые хранит\n"
     "аппарат, и никогда не печатает ключ или тело сертификата.\n"
     "Страница ACME показывает, включён ли автоматический выпуск и что\n"
     "сделал последний заказ; включение, выключение и заказ проходят\n"
     "подготовку там же. Заказ начинается только тогда, когда у аппарата\n"
     "есть пригодное время, готовый слушатель проверки и личность учётной\n"
     "записи; до этого запрос остаётся ожидающим, а панель называет\n"
     "причину вместо успеха, которого не было."},
    {"help.topic.certificates.related",
     "RELATED: PANEL SECURITY, CERTIFICATES, ACME; CONSOLE APPLIANCE",
     "СВЯЗАНО: РАЗДЕЛЫ ПАНЕЛИ БЕЗОПАСНОСТЬ, СЕРТИФИКАТЫ, ACME; КОНСОЛЬ APPLIANCE"},
    {"help.topic.names.title", "Names and routing",
     "Имена и маршрутизация"},
    {"help.topic.names.summary",
     "public names, the route table and the certificate a name gets",
     "публичные имена, таблица маршрутов и сертификат для имени"},
    {"help.topic.names.body",
     "The Routing section configures the facade: one route per public name,\n"
     "with the backend host and port and whether TLS is used. A stage holds\n"
     "the candidate route and the single-use token commits it; a name with\n"
     "no route is refused rather than sent somewhere else.\n"
     "The committed table is written to the state volume before it replaces\n"
     "the applied table, so the table the page shows as applied is the one\n"
     "the next boot carries. The certificate the client is offered follows\n"
     "the name the client asked for, so one appliance can serve several\n"
     "names with their own certificates. A routed name without its own\n"
     "certificate is refused and never lent another name's certificate.",
     "Раздел «Маршрутизация» настраивает фасад: один маршрут на публичное имя,\n"
     "с узлом и портом серверной части и признаком TLS. Подготовка держит\n"
     "маршрут-кандидат, а одноразовый токен его фиксирует; имя без маршрута\n"
     "отклоняется, а не отправляется куда-то ещё.\n"
     "Зафиксированная таблица пишется в том состояния до того, как заменит\n"
     "действующую, поэтому таблица, показанная на странице как действующая,\n"
     "та же, что понесёт следующая загрузка. Сертификат, предложенный\n"
     "клиенту, следует за именем, которое клиент назвал, поэтому один\n"
     "аппарат может обслуживать несколько имён с их сертификатами. Имя с\n"
     "маршрутом, но без собственного сертификата, отклоняется и никогда не\n"
     "одалживает сертификат другого имени."},
    {"help.topic.names.related",
     "RELATED: PANEL ROUTING, CERTIFICATES, NETWORK",
     "СВЯЗАНО: РАЗДЕЛЫ ПАНЕЛИ МАРШРУТИЗАЦИЯ, СЕРТИФИКАТЫ, СЕТЬ"},
    {"help.topic.panel.title", "The management panel",
     "Панель управления"},
    {"help.topic.panel.summary",
     "how to sign in, what a section can change and why there is no shell",
     "как войти, что может менять раздел и почему здесь нет оболочки"},
    {"help.topic.panel.body",
     "The panel is served over HTTPS on the management interface only. Sign\n"
     "in with the administrator login and password; a successful sign-in\n"
     "sets one HttpOnly, Secure, SameSite=Strict session cookie, and a\n"
     "client may present the same session as an Authorization: Bearer\n"
     "header. There is no script on any page, so the panel cannot run code\n"
     "in your browser.\n"
     "No section changes anything by itself. A change is staged, the page\n"
     "publishes the pending mutation and its single-use token, and only the\n"
     "confirming request that repeats the exact token writes. A section\n"
     "that has nothing to change says so. The panel is the whole remote\n"
     "surface: there is no shell, no package manager and no interpreter on\n"
     "the appliance's outer perimeter.",
     "Панель отдаётся по HTTPS только на интерфейсе управления. Войдите под\n"
     "логином и паролем администратора; успешный вход ставит одну сеансовую\n"
     "куку HttpOnly, Secure, SameSite=Strict, а клиент может предъявить тот\n"
     "же сеанс заголовком Authorization: Bearer. Ни на одной странице нет\n"
     "сценариев, поэтому панель не может выполнить код в вашем браузере.\n"
     "Ни один раздел не меняет что-либо сам. Изменение проходит подготовку,\n"
     "страница публикует ожидающую мутацию и её одноразовый токен, и пишет\n"
     "только подтверждающий запрос, повторивший точный токен. Раздел,\n"
     "которому нечего менять, так и говорит. Панель — вся удалённая\n"
     "поверхность: на внешнем периметре аппарата нет оболочки, менеджера\n"
     "пакетов и интерпретатора."},
    {"help.topic.panel.related",
     "RELATED: PANEL DASHBOARD, SYSTEM, ACCOUNT; CONSOLE LOGIN",
     "СВЯЗАНО: РАЗДЕЛЫ ПАНЕЛИ ОБЗОР, СИСТЕМА, УЧЁТНАЯ ЗАПИСЬ; КОНСОЛЬ LOGIN"},
    {"help.topic.recovery.title", "Recovery and the local menu",
     "Восстановление и местное меню"},
    {"help.topic.recovery.summary",
     "the menu offered on the machine and the way back from a bad state",
     "меню на самой машине и путь назад из плохого состояния"},
    {"help.topic.recovery.body",
     "On an idle console the appliance offers the local menu; ENTER opens\n"
     "it and its six entries answer to s (status), n (network),\n"
     "d (diagnostics), r (reboot), p (power off) and y (recovery). The\n"
     "menu works without the network and without a signed-in session,\n"
     "because it is the way back when the panel cannot be reached.\n"
     "The recovery entry does not act on this build: the sealed recovery\n"
     "volume and its confirmation are not bound to the menu, so it answers\n"
     "UNAVAILABLE instead of pretending it repaired something. The\n"
     "break-glass credential reset is the console's CREDENTIALS RESET pair,\n"
     "and a clean reinstall from the medium is the path that rebuilds the\n"
     "machine from scratch.",
     "На консоли без работы аппарат предлагает местное меню; ENTER его\n"
     "открывает, и шесть его пунктов отвечают на s (состояние), n (сеть),\n"
     "d (диагностика), r (перезагрузка), p (выключение) и y\n"
     "(восстановление). Меню работает без сети и без открытого сеанса,\n"
     "потому что это путь назад, когда панель недоступна.\n"
     "Пункт восстановления в этой сборке не действует: запечатанный том\n"
     "восстановления и его подтверждение к меню не привязаны, поэтому он\n"
     "отвечает НЕДОСТУПНО, а не делает вид, что что-то починил. Аварийный\n"
     "сброс учётной записи — пара CREDENTIALS RESET на консоли, а чистая\n"
     "переустановка с носителя — путь, который собирает машину заново."},
    {"help.topic.recovery.related",
     "RELATED: PANEL RECOVERY, DIAGNOSTICS; CONSOLE CREDENTIALS, MENU, BOOTLOG",
     "СВЯЗАНО: РАЗДЕЛЫ ПАНЕЛИ ВОССТАНОВЛЕНИЕ, ДИАГНОСТИКА; КОНСОЛЬ CREDENTIALS, MENU, BOOTLOG"},
    {"help.topic.records.title", "Where the records are",
     "Где смотреть записи"},
    {"help.topic.records.summary",
     "serial, the durable journal, the boot log and the Diagnostics page",
     "последовательный порт, журнал, журнал загрузки и раздел диагностики"},
    {"help.topic.records.body",
     "Machine records are English on purpose: they are the evidence the\n"
     "proofs read, and they do not change with the interface language.\n"
     "They leave the appliance on the serial console, where every line is\n"
     "prefixed by its owner ([CONSOLE], [PRODUCTION-...], [GLYPH-MISSING]\n"
     "and so on), and the durable journal keeps them in the boot log file\n"
     "on the EFI system partition.\n"
     "The BOOTLOG command prints the records the running machine has staged\n"
     "in its memory ring, and APPLIANCE prints the bounded state snapshot -\n"
     "identity, configuration, health, network and services - with the\n"
     "reason for every part that is not OK. The Diagnostics section of the\n"
     "panel has no read owner bound yet and says that, instead of showing\n"
     "an invented log.",
     "Машинные записи намеренно английские: это улики, которые читают\n"
     "пробы, и они не меняются вместе с языком интерфейса. Они покидают\n"
     "аппарат через последовательную консоль, где каждая строка помечена\n"
     "своим владельцем ([CONSOLE], [PRODUCTION-...], [GLYPH-MISSING] и так\n"
     "далее), а долговременный журнал хранит их в файле журнала загрузки на\n"
     "разделе EFI.\n"
     "Команда BOOTLOG печатает записи, которые работающая машина собрала в\n"
     "своём кольце памяти, а APPLIANCE печатает ограниченный снимок\n"
     "состояния — личность, настройку, здоровье, сеть и службы — с причиной\n"
     "для каждой части, которая не в порядке. У раздела «Диагностика»\n"
     "панели ещё нет привязанного владельца чтения, и он так и говорит\n"
     "вместо выдуманного журнала."},
    {"help.topic.records.related",
     "RELATED: PANEL DIAGNOSTICS, DASHBOARD; CONSOLE BOOTLOG, APPLIANCE",
     "СВЯЗАНО: РАЗДЕЛЫ ПАНЕЛИ ДИАГНОСТИКА, ОБЗОР; КОНСОЛЬ BOOTLOG, APPLIANCE"},
    /* ITEM-509 increment 13 (509.7c): the installer screens.  The English
       values are the literals the renderers print today, concatenated exactly
       as the compiler concatenates the adjacent literals, so wiring the
       renderers to these keys cannot move an English screen; nothing reads
       them yet.  The shared account-question module is deliberately not here:
       it is shared with the console and belongs to 509.7d. */
    {"installer.foundation.title", "HFOC Installer", "Установщик HFOC"},
    {"installer.foundation.footer", "W/S move   ENTER select   Q quit",
     "W/S выбор   ENTER подтвердить   Q выход"},
    {"installer.foundation.row_display", "Display", "Дисплей"},
    {"installer.foundation.row_storage", "Storage", "Хранилище"},
    {"installer.foundation.storage_detail", "disk discovery and layout",
     "поиск дисков и разметка"},
    {"installer.foundation.row_network", "Network", "Сеть"},
    {"installer.foundation.network_detail",
     "configuration and reachability", "настройка и доступность"},
    {"installer.foundation.row_continue", "Continue", "Продолжить"},
    {"installer.foundation.continue_detail", "diagnostic flow only",
     "только диагностический поток"},
    {"installer.ui.backend_prefix", "  Backend: ", "  Интерфейс: "},
    {"installer.ui.resolution_prefix", "  Resolution: ", "  Разрешение: "},
    {"installer.ui.pixel_format_prefix", "  Pixel format: ",
     "  Формат пикселей: "},
    {"installer.ui.status_prefix", "  Status: ", "  Состояние: "},
    {"installer.ui.fallback_prefix", "  Fallback: ", "  Резерв: "},
    {"installer.storage.select_disk", "Select installation disk",
     "Выберите диск для установки"},
    {"installer.storage.no_disk", "  No disk was found", "  Диск не найден"},
    {"installer.storage.status_prefix", "  Status: ", "  Состояние: "},
    {"installer.storage.unidentified_disk", "UNIDENTIFIED DISK",
     "ДИСК НЕ ОПОЗНАН"},
    {"installer.storage.transport_prefix", "  Transport: ", "  Шина: "},
    {"installer.storage.table_prefix", "   Table: ", "   Таблица: "},
    {"installer.storage.state_prefix", "  State: ", "  Состояние: "},
    {"installer.storage.existing_system", "   Existing system: YES",
     "   Есть система: ДА"},
    {"installer.storage.efi_partition", "   EFI system partition: YES",
     "   Раздел EFI: ДА"},
    {"installer.storage.confirmed_hint",
     "  Confirmed. ENTER writes it, Q goes back.",
     "  Подтверждено. ENTER записывает, Q назад."},
    {"installer.storage.selected_hint",
     "  Selected. ENTER shows what will be erased.",
     "  Выбрано. ENTER покажет, что будет стёрто."},
    {"installer.storage.move_hint",
     "W/S or arrow keys move   ENTER choose   Q leave",
     "W/S или стрелки — выбор   ENTER — принять   Q — выход"},
    {"installer.storage.chosen_note",
     "Disk chosen by the operator. The warning screen confirms it.",
     "Диск выбран оператором. Экран предупреждения подтверждает его."},
    {"installer.storage.not_chosen_note",
     "No disk is chosen. Select one with ENTER; nothing is chosen for you.",
     "Диск не выбран. Выберите ENTER; за вас ничего не выбирается."},
    {"installer.storage.warning_no_disk", "No disk is selected",
     "Диск не выбран"},
    {"installer.storage.nothing_written", "Nothing will be written.",
     "Ничего записано не будет."},
    {"installer.storage.warning_title",
     "WARNING: EVERYTHING ON THIS DISK WILL BE DELETED",
     "ВНИМАНИЕ: ВСЁ НА ЭТОМ ДИСКЕ БУДЕТ УДАЛЕНО"},
    {"installer.storage.disk_prefix", "  Disk: ", "  Диск: "},
    {"installer.storage.capacity_prefix", "  Capacity: ", "  Ёмкость: "},
    {"installer.storage.warning_transport_prefix", "   Transport: ",
     "   Шина: "},
    {"installer.storage.has_system_note",
     "  An existing system is installed on this disk.",
     "  На этом диске уже есть система."},
    {"installer.storage.has_efi_note",
     "  This disk carries an EFI system partition.",
     "  На этом диске есть раздел EFI."},
    {"installer.storage.no_system_note",
     "  The disk holds no recognised system.",
     "  На диске нет распознанной системы."},
    {"installer.storage.erase_note",
     "  Everything on it - partitions, systems, data - will be erased.",
     "  Всё на нём — разделы, системы, данные — будет стёрто."},
    {"installer.storage.settings_note",
     "  The appliance's own settings are erased too:",
     "  Настройки самого аппарата тоже стираются:"},
    {"installer.storage.settings_detail",
     "  network address, administrator credential, certificates.",
     "  сетевой адрес, учётная запись администратора, сертификаты."},
    {"installer.storage.not_written_yet",
     "  Nothing has been written yet.", "  Пока ничего не записано."},
    {"installer.storage.erase_footer",
     "ENTER erase this disk and install   Q go back",
     "ENTER — стереть диск и установить   Q — назад"},
    {"installer.storage.partition_title", "Give up one existing partition",
     "Отдать один существующий раздел"},
    {"installer.storage.free_space_prefix", "  Free space: ",
     "  Свободно: "},
    {"installer.storage.mb_needed_middle", " MB   Needed: ",
     " МБ   Нужно: "},
    {"installer.storage.mb_suffix", " MB", " МБ"},
    {"installer.storage.partition_not_enough",
     "  Not enough free space: give up a partition, or Q to try free space "
     "anyway.",
     "  Мало свободного места: отдайте раздел или Q — продолжить."},
    {"installer.storage.partition_none",
     "  This disk has no partition that can be given up",
     "  На этом диске нет раздела, который можно отдать"},
    {"installer.storage.partition_unchanged",
     "  Nothing was changed. Q continues with free space.",
     "  Ничего не изменено. Q продолжит без раздела."},
    {"installer.storage.partition_prefix", "Partition ", "Раздел "},
    {"installer.storage.partition_system_prefix", "     System: ",
     "     Система: "},
    {"installer.storage.partition_move_hint",
     "W/S or arrow keys move   ENTER choose   Q keep every partition",
     "W/S или стрелки — выбор   ENTER — принять   Q — сохранить разделы"},
    {"installer.storage.partition_keep_note",
     "Q keeps every partition and continues with free space.",
     "Q сохраняет все разделы и продолжает без них."},
    {"installer.storage.partition_warning_no_partition",
     "No partition is selected", "Раздел не выбран"},
    {"installer.storage.partition_warning_title",
     "WARNING: THE DATA ON THIS PARTITION WILL BE DELETED",
     "ВНИМАНИЕ: ДАННЫЕ В ЭТОМ РАЗДЕЛЕ БУДУТ УДАЛЕНЫ"},
    {"installer.storage.partition_field_prefix", "  Partition: ",
     "  Раздел: "},
    {"installer.storage.size_prefix", "   Size: ", "   Размер: "},
    {"installer.storage.type_prefix", "   Type: ", "   Тип: "},
    {"installer.storage.layout_prefix", "  Layout: ", "  Разметка: "},
    {"installer.storage.system_prefix", "   System: ", "   Система: "},
    {"installer.storage.disk_field_prefix", "   Disk: ", "   Диск: "},
    /*
     * The warning screen writes this sentence as two screen lines, so the
     * catalog carries the two lines: joining them into one key would print a
     * different screen and a longer line than the English one.
     */
    {"installer.storage.partition_removed_note",
     "  This partition is removed and the appliance's three",
     "  Этот раздел удаляется, а на его месте"},
    {"installer.storage.partition_created_note",
     "  partitions are created in the space it occupied.",
     "  создаются три раздела аппарата."},
    {"installer.storage.partition_others_note",
     "  Every other partition, the shared EFI System",
     "  Все остальные разделы, общий раздел EFI"},
    {"installer.storage.partition_others_detail",
     "  Partition and the systems on them stay untouched.",
     "  и системы на них остаются нетронутыми."},
    {"installer.storage.partition_footer",
     "ENTER give up this partition and install   Q go back",
     "ENTER — отдать раздел и установить   Q — назад"},
    /*
     * ITEM-514: the install scheme screen and the summary before the write.
     * The scheme names and their one-line descriptions are the list the
     * operator reads; the summary's keys are the last page before the first
     * write, and the boundary sentence (no full manual partitioning, no
     * shrink, no LVM or RAID) is part of the list on purpose.
     */
    {"installer.scheme.title", "Choose the installation scheme",
     "Выберите схему установки"},
    {"installer.scheme.alongside", "Install alongside the existing system",
     "Установить рядом с существующей системой"},
    {"installer.scheme.alongside_detail",
     "  Uses free space; existing partitions and systems stay.",
     "  Использует свободное место; разделы и системы остаются."},
    {"installer.scheme.free_space", "Use the free space",
     "Использовать свободное место"},
    {"installer.scheme.free_space_detail",
     "  Uses the largest free region; nothing else is touched.",
     "  Использует наибольший свободный участок; остальное не трогается."},
    {"installer.scheme.erase", "Erase the whole disk and install HFOC",
     "Стереть весь диск и установить HFOC"},
    {"installer.scheme.erase_detail",
     "  DESTRUCTIVE: deletes every partition and all data.",
     "  РАЗРУШИТЕЛЬНО: удаляет все разделы и все данные."},
    {"installer.scheme.manual", "Manual: give up one existing partition",
     "Вручную: отдать один существующий раздел"},
    {"installer.scheme.manual_detail",
     "  No full manual partitioning, no shrink, no LVM or RAID.",
     "  Ручной разметки целиком, сжатия, LVM и RAID нет."},
    {"installer.scheme.move_hint",
     "W/S or arrow keys move   ENTER choose   Q leave",
     "W/S или стрелки — выбор   ENTER — принять   Q — выход"},
    {"installer.scheme.unavailable_prefix", "   unavailable: ",
     "   недоступно: "},
    {"installer.scheme.unknown_contents",
     "  The contents of this disk could not be read: what is on it is unknown.",
     "  Содержимое диска прочитать не удалось: оно неизвестно."},
    {"installer.scheme.refusal_title", "This scheme cannot be used",
     "Эту схему применить нельзя"},
    {"installer.scheme.refusal_reason", "  Reason: ", "  Причина: "},
    {"installer.scheme.refusal_hint",
     "Give up a partition with Manual, or choose another scheme.",
     "Отдайте раздел через «Вручную» или выберите другую схему."},
    {"installer.scheme.refusal_footer",
     "ENTER or Q goes back to the schemes",
     "ENTER или Q — назад к схемам"},
    {"installer.scheme.summary_title", "Confirm the installation plan",
     "Подтвердите план установки"},
    {"installer.scheme.summary_scheme_prefix", "  Scheme: ", "  Схема: "},
    {"installer.scheme.summary_declared_prefix", "   Declared: ",
     "   Объявлено: "},
    {"installer.scheme.summary_chosen_prefix", "   Chosen: ", "   Выбрано: "},
    {"installer.scheme.summary_deleted_prefix", "  Will be deleted: ",
     "  Будет удалено: "},
    {"installer.scheme.summary_delete_all", "everything on this disk",
     "всё на этом диске"},
    {"installer.scheme.summary_delete_none",
     "nothing: the existing partitions stay",
     "ничего: существующие разделы остаются"},
    {"installer.scheme.summary_delete_partition", "partition ", "раздел "},
    {"installer.scheme.summary_created_prefix", "  Will be created:",
     "  Будет создано:"},
    {"installer.scheme.summary_role_prefix", "    ", "    "},
    {"installer.scheme.role.efi", "EFI system partition", "раздел EFI"},
    {"installer.scheme.role.system", "HFOC system", "система HFOC"},
    {"installer.scheme.role.persistence", "HFOC persistence",
     "хранилище HFOC"},
    {"installer.scheme.role.recovery", "HFOC recovery", "восстановление HFOC"},
    /*
     * ITEM-517: the legacy-BIOS layout's two roles.  Neither is an EFI System
     * Partition: the BIOS boot partition carries Limine's stage2 and the FAT
     * boot volume carries the installed payload.
     */
    {"installer.scheme.role.bios_boot", "BIOS boot partition",
     "раздел загрузки BIOS"},
    {"installer.scheme.role.boot_volume", "FAT boot volume",
     "загрузочный том FAT"},
    {"installer.scheme.summary_reused", "  (reused)", "  (переиспользуется)"},
    {"installer.scheme.summary_esp_reused", "  EFI system partition: reused",
     "  Раздел EFI: переиспользуется"},
    {"installer.scheme.summary_esp_created", "  EFI system partition: created",
     "  Раздел EFI: создаётся"},
    {"installer.scheme.summary_kept_prefix", "  Existing systems kept: ",
     "  Сохранённых систем: "},
    {"installer.scheme.summary_menu_added",
     "   Menu entries added: HFOC appliance",
     "   Записей меню добавлено: аппарат HFOC"},
    {"installer.scheme.summary_free_left_prefix", "  Free space left: ",
     "  Останется свободно: "},
    {"installer.scheme.summary_footer",
     "ENTER confirm this plan   Q go back to the schemes",
     "ENTER — подтвердить план   Q — назад к схемам"},
    {"installer.scheme.summary_refused_prefix",
     "  The plan cannot be built: ", "  План построить нельзя: "},
    {"installer.refusal.title", "Installation refused", "Установка отклонена"},
    {"installer.refusal.reason_prefix", "  Reason: ", "  Причина: "},
    {"installer.refusal.medium",
     "  This medium cannot install on this machine.",
     "  Этот носитель не может установить систему на эту машину."},
    {"installer.refusal.press_key",
     "  Press any key to continue.",
     "  Нажмите любую клавишу, чтобы продолжить."},
    {"storage.report.title", "Observed storage", "Наблюдённое хранилище"},
    {"storage.report.controllers", "controllers", "контроллеры"},
    {"storage.report.disks", "disks", "диски"},
    {"storage.report.disk", "disk", "диск"},
    {"storage.report.not_probed", "storage was not probed",
     "хранилище не опрошено"},
    {"storage.report.probe_failed", "storage probe failed",
     "опрос хранилища завершился ошибкой"},
    {"installer.credential.title", "HFOC OS installer - account",
     "Установщик HFOC OS — учётная запись"},
    {"installer.credential.password_note",
     "The password is shown as asterisks as you type.",
     "Пароль показывается звёздочками при вводе."},
    {"installer.credential.checking", "Checking the account...",
     "Проверка учётной записи..."},
    {"installer.notice.title", "HFOC OS INSTALLED", "HFOC OS УСТАНОВЛЕНА"},
    {"installer.notice.remove_medium",
     "REMOVE THE INSTALLATION MEDIUM WHILE THE MACHINE RESTARTS",
     "ИЗВЛЕКИТЕ НОСИТЕЛЬ УСТАНОВКИ, ПОКА МАШИНА ПЕРЕЗАГРУЖАЕТСЯ"},
    {"installer.media.declared_disk_prefix", "Declared disk ",
     "Объявленный диск "},
    {"installer.media.not_on_machine_suffix",
     " is not on this machine - choose a disk",
     " отсутствует на этой машине — выберите диск"}
};

#define APPLIANCE_LANGUAGE_CATALOG_COUNT \
    (sizeof(appliance_language_catalog) / sizeof(appliance_language_catalog[0]))

/*
 * ITEM-509 increment 7: the topics the help renders, in the order the table of
 * contents prints them.  Each one names the four keys its strings live under;
 * the strings are in the catalog above, so they are translated and covered
 * like every other interface sentence.  The console renders this list now and
 * the panel will render the same list in increment 8 - one content, two
 * renderers, so a topic cannot exist on one surface and be missing on the
 * other.
 */
struct appliance_help_topic_keys {
    const char *id;
    const char *keys[APPLIANCE_HELP_FIELD_COUNT];
};

static const struct appliance_help_topic_keys appliance_help_topics[] = {
    {"install",
     {"help.topic.install.title", "help.topic.install.summary",
      "help.topic.install.body", "help.topic.install.related"}},
    {"network",
     {"help.topic.network.title", "help.topic.network.summary",
      "help.topic.network.body", "help.topic.network.related"}},
    {"storage",
     {"help.topic.storage.title", "help.topic.storage.summary",
      "help.topic.storage.body", "help.topic.storage.related"}},
    {"account",
     {"help.topic.account.title", "help.topic.account.summary",
      "help.topic.account.body", "help.topic.account.related"}},
    {"certificates",
     {"help.topic.certificates.title", "help.topic.certificates.summary",
      "help.topic.certificates.body", "help.topic.certificates.related"}},
    {"names",
     {"help.topic.names.title", "help.topic.names.summary",
      "help.topic.names.body", "help.topic.names.related"}},
    {"panel",
     {"help.topic.panel.title", "help.topic.panel.summary",
      "help.topic.panel.body", "help.topic.panel.related"}},
    {"recovery",
     {"help.topic.recovery.title", "help.topic.recovery.summary",
      "help.topic.recovery.body", "help.topic.recovery.related"}},
    {"records",
     {"help.topic.records.title", "help.topic.records.summary",
      "help.topic.records.body", "help.topic.records.related"}}
};

#define APPLIANCE_HELP_TOPIC_COUNT \
    (sizeof(appliance_help_topics) / sizeof(appliance_help_topics[0]))

static size_t language_text_length(const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

static bool language_key_equal(const char *left, const char *right)
{
    size_t index = 0U;

    while (left[index] != '\0' && left[index] == right[index]) {
        ++index;
    }
    return left[index] == right[index];
}

size_t appliance_language_catalog_count(void)
{
    return APPLIANCE_LANGUAGE_CATALOG_COUNT;
}

const char *appliance_language_catalog_key(size_t index)
{
    if (index >= APPLIANCE_LANGUAGE_CATALOG_COUNT) {
        return NULL;
    }
    return appliance_language_catalog[index].key;
}

const char *appliance_language_catalog_value(size_t index,
                                             enum appliance_language language)
{
    if (index >= APPLIANCE_LANGUAGE_CATALOG_COUNT) {
        return NULL;
    }
    switch (language) {
    case APPLIANCE_LANGUAGE_EN:
        return appliance_language_catalog[index].en;
    case APPLIANCE_LANGUAGE_RU:
        return appliance_language_catalog[index].ru;
    case APPLIANCE_LANGUAGE_COUNT:
    default:
        return NULL;
    }
}

const char *appliance_language_code(enum appliance_language language)
{
    switch (language) {
    case APPLIANCE_LANGUAGE_EN:
        return "en";
    case APPLIANCE_LANGUAGE_RU:
        return "ru";
    case APPLIANCE_LANGUAGE_COUNT:
    default:
        return NULL;
    }
}

enum appliance_language_status appliance_language_parse(
    const char *text, size_t length, enum appliance_language *language)
{
    if (text == NULL || language == NULL) {
        return APPLIANCE_LANGUAGE_UNKNOWN_LANGUAGE;
    }
    if (length == 2U && text[0] == 'e' && text[1] == 'n') {
        *language = APPLIANCE_LANGUAGE_EN;
        return APPLIANCE_LANGUAGE_OK;
    }
    if (length == 2U && text[0] == 'r' && text[1] == 'u') {
        *language = APPLIANCE_LANGUAGE_RU;
        return APPLIANCE_LANGUAGE_OK;
    }
    return APPLIANCE_LANGUAGE_UNKNOWN_LANGUAGE;
}

const char *appliance_language_string(enum appliance_language language,
                                      const char *key)
{
    size_t index;

    if (key == NULL) {
        return NULL;
    }
    for (index = 0U; index < APPLIANCE_LANGUAGE_CATALOG_COUNT; ++index) {
        if (language_key_equal(appliance_language_catalog[index].key, key)) {
            const char *value =
                appliance_language_catalog_value(index, language);

            /* An absent or empty translation is a refusal, not a fallback. */
            if (value == NULL || value[0] == '\0') {
                return NULL;
            }
            return value;
        }
    }
    return NULL;
}

bool appliance_language_utf8_next(const char *text, size_t length,
                                  size_t *offset, uint32_t *codepoint)
{
    const unsigned char *bytes = (const unsigned char *)text;
    size_t index;
    size_t extra;
    size_t position;
    uint32_t value;

    if (text == NULL || offset == NULL || codepoint == NULL) {
        return false;
    }
    index = *offset;
    if (index >= length) {
        return false;
    }
    if (bytes[index] < 0x80U) {
        *codepoint = bytes[index];
        *offset = index + 1U;
        return true;
    }
    if ((bytes[index] & 0xE0U) == 0xC0U) {
        value = bytes[index] & 0x1FU;
        extra = 1U;
        if (value < 0x02U) {
            return false; /* overlong */
        }
    } else if ((bytes[index] & 0xF0U) == 0xE0U) {
        value = bytes[index] & 0x0FU;
        extra = 2U;
    } else if ((bytes[index] & 0xF8U) == 0xF0U) {
        value = bytes[index] & 0x07U;
        extra = 3U;
        if (value > 0x04U) {
            return false; /* beyond U+10FFFF */
        }
    } else {
        return false; /* continuation byte or invalid lead */
    }
    if (index + extra >= length) {
        return false; /* truncated */
    }
    for (position = 1U; position <= extra; ++position) {
        unsigned char continuation = bytes[index + position];

        if ((continuation & 0xC0U) != 0x80U) {
            return false;
        }
        value = (value << 6) | (uint32_t)(continuation & 0x3FU);
    }
    if ((extra == 2U && value < 0x800U) ||
        (extra == 3U && value < 0x10000U) ||
        (value >= 0xD800U && value <= 0xDFFFU) || value > 0x10FFFFU) {
        return false; /* overlong, surrogate or beyond the codespace */
    }
    *codepoint = value;
    *offset = index + extra + 1U;
    return true;
}

size_t appliance_language_ru_codepoints(uint32_t *codepoints, size_t capacity)
{
    size_t count = 0U;
    size_t index;

    if (codepoints == NULL || capacity == 0U) {
        return (size_t)-1;
    }
    for (index = 0U; index < APPLIANCE_LANGUAGE_CATALOG_COUNT; ++index) {
        const char *value = appliance_language_catalog[index].ru;
        size_t length = language_text_length(value);
        size_t offset = 0U;

        while (offset < length) {
            uint32_t codepoint;
            size_t insert;
            size_t move;

            if (!appliance_language_utf8_next(value, length, &offset,
                                              &codepoint)) {
                return (size_t)-1;
            }
            for (insert = 0U;
                 insert < count && codepoints[insert] < codepoint; ++insert) {
            }
            if (insert < count && codepoints[insert] == codepoint) {
                continue; /* already in the set */
            }
            if (count == capacity) {
                return (size_t)-1; /* never a partial set that looks complete */
            }
            for (move = count; move > insert; --move) {
                codepoints[move] = codepoints[move - 1U];
            }
            codepoints[insert] = codepoint;
            ++count;
        }
    }
    return count;
}

static char language_help_fold(char value)
{
    if (value >= 'A' && value <= 'Z') {
        return (char)(value - 'A' + 'a');
    }
    return value;
}

size_t appliance_help_topic_count(void)
{
    return APPLIANCE_HELP_TOPIC_COUNT;
}

const char *appliance_help_topic_id(size_t index)
{
    if (index >= APPLIANCE_HELP_TOPIC_COUNT) {
        return NULL;
    }
    return appliance_help_topics[index].id;
}

const char *appliance_help_topic_key(size_t index,
                                     enum appliance_help_field field)
{
    if (index >= APPLIANCE_HELP_TOPIC_COUNT ||
        (size_t)field >= (size_t)APPLIANCE_HELP_FIELD_COUNT) {
        return NULL;
    }
    return appliance_help_topics[index].keys[field];
}

const char *appliance_help_topic_string(size_t index,
                                        enum appliance_help_field field,
                                        enum appliance_language language)
{
    return appliance_language_string(language,
                                     appliance_help_topic_key(index, field));
}

size_t appliance_help_topic_find(const char *id, size_t length)
{
    size_t index;

    if (id == NULL || length == 0U) {
        return (size_t)-1;
    }
    for (index = 0U; index < APPLIANCE_HELP_TOPIC_COUNT; ++index) {
        const char *candidate = appliance_help_topics[index].id;
        size_t position = 0U;

        while (position < length && candidate[position] != '\0' &&
               language_help_fold(candidate[position]) ==
                   language_help_fold(id[position])) {
            ++position;
        }
        if (position == length && candidate[position] == '\0') {
            return index;
        }
    }
    return (size_t)-1;
}
