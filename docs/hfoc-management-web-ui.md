# HFOC Management Web UI

**Date:** 2026-09-06, extended 2026-09-11 (ITEM-384)
**Target:** ITEM-345 of
`docs/superpowers/plans/2026-09-06-hfoc-appliance-installer-and-management-ui.md`
and ITEM-384 of
`docs/superpowers/plans/2026-09-11-appliance-productization-queue.md`
**Base SHA:** `c6e4b3227965b2be717702818a89dd17c25d1b8a`
**Prerequisite boundaries:** ITEM-315..322 management API and security;
ITEM-326 appliance health; ITEM-328 health API; ITEM-344 local appliance
console; ITEM-366/367A management plane and mutation plane; ITEM-375
management TLS
**Status:** ITEM-345 shell completed by ITEM-345; rendered panel, session
flow, hardening headers and asset budget completed by ITEM-384

## Purpose

This document is the authoritative management web UI boundary.
HFOC Appliance is a headless server operating system; the primary
administration surface is intended to be an HTTPS management panel. ITEM-345
added a compact management shell that identifies the appliance and exposes
the required administration sections without duplicating management API
business logic. ITEM-384 turns that shell into server-side rendered HTML
pages, adds the panel sign-in/session presentation, publishes the two-phase
confirmation token for mutations, and hardens the management listener with a
Content-Security-Policy and a measured asset budget.

The panel is served and proved over the real TLS 1.3 HTTPS QEMU fixture. It
does not claim production-browser or physical-hardware acceptance.

## Modules

`appliance_management_web_ui` lives under `kernel/` and publishes:

- the anonymous sign-in document served on the root target;
- one rendered page per management section;
- exact section IDs and strings for Dashboard, Network, Services, Storage,
  System, Security, Diagnostics, Updates, Recovery, Routing and Account;
- exact panel targets (`/panel`, `/panel/network`, ...) as one static table
  owned by the UI, dispatched by `appliance_management_panel`; the navigation
  links are printed from that same table, which is the single source of truth,
  so the menu cannot publish a target the dispatcher does not match (ITEM-499);
- ownership and state for every section: `AVAILABLE` when a read-side owner is
  bound, `DEGRADED` when the owner is a mutation-only surface, and
  `UNAVAILABLE` when no owner exists at all;
- an allocation-bounded HTML escaper and a deterministic renderer with
  NUL-capacity and overflow handling;
- the recorded per-page asset budget;
- no allocation, storage I/O, network I/O, audit write, or control mutation.

`appliance_management_panel` lives under `kernel/` and publishes the panel
dispatch: it maps a panel target to a section, admits the request through the
existing ITEM-365 production auth composition (the ITEM-320 administrator
session boundary plus the ITEM-321 authorization matrix, deny by default),
publishes the throttle window of the existing administrator authentication
boundary, and renders the sign-in document or the section document. The panel
owns no session, no credential, no route registration, and no authorization
rule of its own.

## Rendered documents

The root target `/` serves the anonymous sign-in document. It is a complete
HTML document with an inline stylesheet, a plain-text index of the ten
sections, the sign-in state (`SIGNED OUT`, `SIGNED IN`, or `THROTTLED`), the
session identity when one is presented, the throttle window when the
administrator authentication boundary is locked out, the sign-in notice, the
one same-origin sign-in form (ITEM-445), and one line that says what to do
before the form can work (ITEM-454, ITEM-506): the account is set on the
machine's own console with `credentials`, and the same login and password are
typed into the form. It contains no browser script and no external asset.

Each panel target serves one section document: the same head, a link
navigation, the section name, the section state, the owning management API
path, the dynamic values the caller supplies (hostname, management MAC,
service name, detail, recent log line), and — for the sections that own a
mutation surface — the two-phase confirmation block. The Routing page is the
exception that proves the rule: it carries no read-side value dump, because
its state is the applied route table the route configuration published and the
page lists that table instead.

The navigation is rendered from the section path accessor itself: every `href`
is the declared target, printed once and printed as it is, and the dispatcher
matches a request against that same accessor. ITEM-499 fixed the writer that
prefixed `/panel` and then appended the whole declared path, which published
`/panel//panel/network` while the dispatcher matched `/panel/network`: the
panel opened, every menu entry but Dashboard answered `404 not_found`, and the
owner saw the refusal document. Both sides of the proof read the rendered
documents rather than the source: the host proof extracts every published
`href` and form `action` and requires each target to be one the machine serves,
and the QEMU proof walks the links of the served document as a remote client —
each target answers the sign-in document anonymously (`401`, never `404`) and
its own page with the session. A section published as `UNAVAILABLE` names the
reason on the page (`no read side is published for it`, or the mutation-only
surface for `DEGRADED`) instead of leaving the reader at the word.

## Session flow and its documented limits

The panel session flow uses the existing authorization matrix, unchanged:

- the session credential is the existing 32-byte administrator session token,
  issued by the local administration console through
  `appliance_admin_auth_authenticate_local()`. The panel never issues a
  session, never accepts a password, and adds no authentication factor;
- the token is presented in the `Authorization: Bearer <token>` request
  header, exactly as the management API presents it. The panel reads nothing
  else. The parser publishes a header as a name without the colon and a value
  that keeps the whitespace after it, so the wire path rebuilds the canonical
  `Authorization: Bearer <32 bytes>` line before handing it to the existing
  boundary instead of teaching the boundary a second header format;
- the request is admitted by `appliance_production_auth_authorize_request()`
  against the management authorization matrix. An unknown panel target, a
  missing or malformed header, a wrong token, an expired or revoked session,
  a wrong role, or a missing capability is denied by default;
- the sign-in state and the throttle window are published from the existing
  `appliance_admin_auth` failure counter and lockout tick. The panel does not
  implement a second throttle and does not reset the existing one.

## Browser sign-in (ITEM-445)

Until ITEM-445 the panel was reachable only by a client that could send
`Authorization: Bearer <token>`, and the token was issued by the local
administration console, so a browser could not sign in at all. The owner's
decision (2026-09-18, option 1) adds exactly two things and keeps the page
script-free:

1. the sign-in document carries one form (`<form method="post" action="/">`)
   with the two declared fields `username` and `password`, and it posts to the
   same origin. The in-kernel HTTP parser accepts a request body only for
   `POST /` and `POST /panel/logout`, and only up to
   `HTTP_FRAMING_BODY_MAX` (512) bytes; a body on any other method or target,
   a duplicate `Content-Length`, `Transfer-Encoding` or `Expect` stays denied
   exactly as before. Both receive loops collect exactly the declared bytes
   before the request completes, so a body byte is never parsed as a second
   request;
2. a successful sign-in answers `303 See Other` with `Location:
   /panel` and one `Set-Cookie` value:
   `hfoc_session=<64 lower-case hex>; HttpOnly; Secure; SameSite=Strict;
   Path=/; Max-Age=900`. The cookie carries the token the existing
   administrator boundary issued, and a section request with no
   `Authorization` header rebuilds that token into the same canonical bearer
   line, so there is still one session store and one expiry rule. A cookie
   whose value is not exactly 64 hex digits, or two `hfoc_session` cookies in
   one header, is treated as unsigned rather than resolved by guessing.
   `/panel/logout` answers the same cookie with an empty value and
   `Max-Age=0`.

ITEM-513 (owner's decision 2026-10-10) adds the second and last cookie: the
panel's language is the browser's own property. Every panel page's nav carries
one POST form with two submit buttons, `EN` and `RU` (labels from the catalog);
a click submits exactly the one field `language=en|ru` to `/panel/language`,
which answers `303 See Other` with `Location: /panel` - the Dashboard target
the section model actually serves; ITEM-513 corrected the sign-in destination
from `/panel/dashboard`, which no target served and which the browser reached
as a 404 - and
`Set-Cookie: hfoc_lang=<en|ru>; HttpOnly; Secure; SameSite=Strict; Path=/`, or
a named `PANEL_LANGUAGE_BAD_VALUE` (`400`, no cookie) for any other body. The
switch needs no token and no second phase: it changes no machine state. The
request's language is resolved once, in this order: a valid `hfoc_lang` cookie,
then the shared `v1:system.language` record, then `en`. A cookie the reader
refuses (empty, unknown, upper-case, duplicated) is a named refusal — counted
and answered with the record's language and a clearing
`hfoc_lang=; …; Max-Age=0` cookie — never a half-translated page and never a
guess. `v1:system.language`, the console's `language` command and the System
page's two-phase language form are untouched and remain the machine's default
for a browser without the cookie.

The form consults the same `appliance_production_admin_enrollment` owner the
local console uses, through a credential boundary the serving path binds at
boot; the panel derives no verifier, keeps no secret beyond the parsed form
buffer (which is zeroed after the proof is derived), and implements no second
throttle. The content security policy allows `form-action 'self'` — the one
attribute that changed — and still carries no `script-src`, so the page cannot
run a script. Denials keep their statuses: a malformed, partial or
wrong-content-type form is answered `400` with the sign-in document, a wrong
login or password `401` without a cookie, a locked-out boundary `429` with its
remaining window, and a panel with no credential owner `503`.

Denials are published, never hidden: an unauthenticated panel request returns
`401` with the sign-in document, a throttled one returns `429` with the
throttle window, a denied one returns `403`, and an unpublished management
plane returns `503`. Every response body is a real rendered document.

### The sign-in document explains itself (ITEM-454, ITEM-506)

The owner reached this page, and could not sign in: nothing on it said where the
account is set, and the console's `help` printed the command in upper case while
the parser accepted only the lower-case spelling - so an operator who typed
exactly what help showed was told `UNKNOWN COMMAND`. ITEM-454 fixed the case
rule; ITEM-506 replaced the account and the commands:

* the sign-in document (the static shell body and the rendered login page, byte
  for byte the same document) carries one line naming the account command:
  `credentials`. The form has posted `username` and `password` since ITEM-445,
  and ITEM-506 made the login half real: the panel passes both fields to the
  enrollment owner, which compares the pair with the one stored account. The
  host proof requires the sentence, and the QEMU response matrices compare the
  whole document, so a copy that drifts fails;
* the console has one account vocabulary
  (`kernel/include/console_credentials_command.h`): `credentials` starts the
  change conversation (current password, new login, new password twice) and
  `login` opens the local session; neither takes an argument, so a password
  never reaches the input history or a command record. The old `admin`,
  `admin-provision` and `admin-login` names are removed, not hidden, and the
  host proof drives the classifier and the whole conversation;
* the break-glass reset stays under the same family: `CREDENTIALS RESET` arms
  it and `CREDENTIALS RESET CONFIRM` on the next line performs it; the reset
  rewrites the persisted record to the factory placeholder and closes every
  issued session, so `credentials` can set the account again. The panel
  deliberately exposes no reset route and no network client can reach the
  gesture - see `docs/hfoc-production-admin-enrollment.md`.

The measured login asset budget moved with the sentence (1613 -> 1585 bytes at
that item; ITEM-506 increment 6 grew it to 1593 with the Account link, still
inside the then-2048-byte body cap; ITEM-509 increment 8 raised that
cap to 3328), and every recorded `Content-Length` for the
served document moved with each change.

## Two-phase confirmation

Mutations stay two-phase and single-use. The panel never confirms a mutation;
it publishes the pending confirmation so an operator can present the token in
the confirming request:

```
Mutation: UPDATE_ACCEPT
Confirmation token: 1099511627776 (single-use)
Expires in: 271 ticks. Confirm by repeating the mutation request with this
exact token inside the window; a second confirmation is refused.
```

A section without a mutation surface never renders a token, and a section with
no pending mutation says so explicitly.

## The Security page imports the operator certificate (ITEM-503)

The Security section has no read owner, and it says so: its page publishes the
UNAVAILABLE state with the sentence that the certificate import form below is
the section's mutation surface. The form is plain HTML, posts to its own
origin (`/panel/security`, `form-action 'self'`, no script) and carries three
textareas — `certificate`, optional `chain` (a bundle of PEM certificates) and
`key` — plus a second form that repeats the single-use confirmation token or
cancels the pending candidate. The page renders the pending token only while a
candidate waits; it does not repeat the generic prose block, because the form
and the token share one page.

The route is the same mutation the API target `POST /api/v1/system/certificate`
resolves to: `CERTIFICATE_IMPORT`, command id 12, admin role with the
configuration-write capability. A cookie session and an `Authorization: Bearer`
header are the same principal. The panel answer to a staged import is `303 See
Other` back to the page; the API answer is one JSON line carrying the state
record and the pending token. A refusal is answered with the named reason
(`BAD_PAIR`, `BAD_CLOCK`, `TOO_LARGE`, …) and never stores anything.

The measured Security page budget moved from 1420 to 1883 bytes with the form
and the pending-confirmation block.

## The Routing page configures the facade (ITEM-504)

The Routing section is the panel's first mutating section. Its page lists the
applied table the route configuration published — one line per route, byte for
byte as `appliance_route_render` writes the record
(`name=… backend=…:port tls=… source=…`) — then offers the stage form and the
confirmation form. The stage form posts `action` (`add` or `remove`), `name`,
`backend`, `port` and `tls` to `/panel/routing`; the second form repeats the
single-use `token` with `confirm`, or discards the candidate with `cancel`.
The route is the same mutation the API target `POST /api/v1/system/routes`
resolves to: `ROUTING_CONFIG`, command id 13, admin role with the
configuration-write capability, and a cookie session is the same principal as
an `Authorization: Bearer` header.

The page is honest about what it does not know. A table its owner never
published is named as unpublished, never rendered as an empty one; a published
table with no routes says every public name is refused; a table too large for
the page's bounded copy is counted and flagged (`N routes applied, M of them
listed here`) instead of being cut silently; and every byte of the table passes
through the escaper, so a hostile name cannot inject markup. The pending
candidate is one line — mutation, token, remaining window in milliseconds (the
unit the plane compares) and single-use — because the table and the forms
share one page.

The measured Routing page budget is 1994 bytes (ITEM-506 increment 6 added the
Account link to every page's navigation); see the table above. Applying
the table to live traffic is the next increment: this page configures the table
the facade will route with.

The table the page lists is also the table the appliance keeps. Since ITEM-504
increment 6 the routing commit writes the candidate into the state volume's own
route store before it replaces the applied table, so the commit the page shows
as applied is the one the next boot carries; a store that cannot prove the
record fails the commit and the page keeps the previous table. The boot reads
that record back and publishes the whole table it restored as
`[PRODUCTION-ROUTES] source=…` (`COMMITTED`, `DEFAULT_MISSING`,
`DEFAULT_INVALID` or `DEFAULT_UNAVAILABLE`), so an empty configuration is
distinguishable from a record that could not be trusted. The page itself still
shows the applied table; the stored one is what the next boot will apply.

The change itself is recorded on the machine, not only answered to the client:
every terminal state of the routing mutation leaves one bounded record —
`result=APPLIED` with the applied route, `result=CANCELLED` with the dropped
candidate, or `result=REFUSED reason=…` with the requested name and the
model's own reason — and the boot's loop drains it as `[PRODUCTION-ROUTE]` into
the serial and durable logs. A stage that only holds a candidate records
nothing until it is committed or cancelled. The record is built next to the
state that decided it; the panel and the HTTP target only carry the outcome to
the operator, so the log cannot disagree with the table.

## Escaping

Every dynamic value shown by a page (hostname, MAC address, service name,
detail, log line, notice, owner path, mutation name) is HTML-escaped before it
is appended: `&`, `<`, `>`, `"`, and `'` become entities and every control
byte becomes a numeric reference. A hostile value can therefore not close a
tag, open an attribute, or inject markup into a document served from the
management origin. An injection in the panel would be a management-plane
compromise, because the panel is served with an administrator session on the
management listener.

## Hardening headers

Every management listener response — success and error alike — carries the
fixed head:

```
Cache-Control: no-store
Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'
Strict-Transport-Security: max-age=31536000
Referrer-Policy: no-referrer
X-Content-Type-Options: nosniff
X-Frame-Options: DENY
```

The policy allows no script, no image, no connection and no frame, and its one
allowed form target is `'self'` — the same-origin sign-in and sign-out forms
ITEM-445 adds. `style-src 'unsafe-inline'` covers the one inline stylesheet the
panel ships; because script execution stays forbidden and every dynamic value
is escaped, an injected style rule is the only reachable consequence of a
rendering defect.

## The Account page changes the one account (ITEM-506 increment 6)

The Account section is the panel's fourth configuring section (after Network,
System and Routing). Its page shows the
stored login (never the password), then offers the four-field stage form
(`current`, `login`, `password`, `repeat`) and the confirmation form. The stage
form posts to `/panel/account`; the second form repeats the single-use `token`
with `confirm`, or discards the candidate with `cancel`. The change is the same
mutation the API target `POST /api/v1/system/account` resolves to:
`ACCOUNT_CREDENTIAL`, command id 15, admin role with the configuration-write
capability, and a cookie session is the same principal as an
`Authorization: Bearer` header.

The page is honest about what it does not know. A login its owner never
published is named as unpublished, never rendered as a blank; the page states
that a change applies immediately and signs out every session, including the
one that confirms it; and every dynamic value passes through the escaper. The
candidate's plaintext lives only in the owner's bounded buffer for the
confirmation window (300 s) and is zeroed by the commit, by a cancel, by expiry
and by a new stage; the page and the record never carry it. The operator can
also change the account on the machine console with `credentials`, and the
break-glass `credentials reset` stays console-only.

The measured Account page budget is 1957 bytes; see the table above.

## Help

ITEM-509 increment 8 publishes the Help section: `/panel/help` is the table of
contents and `/panel/help/<id>` is one topic. The text is the console's help
catalog - one topic table with four strings per topic - so the panel renderer
and the console renderer cannot tell different stories. A topic page prints the
title, the summary, the body line by line (each author break becomes `<br>`) and
the related commands and sections; the Dashboard carries a short `Help index`
block with the same links. The section names no management API owner: the
catalog is compiled into the build, and the page says so. The language comes
from the same system record the System page publishes; a record that did not
load, an unknown topic id and an incomplete catalog are named refusals
(`SYSTEM_LANGUAGE_UNAVAILABLE`, `NO_SUCH_HELP_TOPIC`, `CATALOG_INCOMPLETE` with
the offending key), never an empty page and never a silent English fallback.
`<html lang>` still describes the panel's own English chrome and moves with the
panel translation (509.7); the help body itself is rendered in the language the
machine is set to.

## Asset budget

The budget is the byte count of HTML plus inline CSS actually served for a
page. It is measured by `tests/appliance-management-web-ui-host.c`, recorded
in `kernel/include/appliance_management_web_ui.h`, and repeated in the table
below; the host test fails when a page grows past its recorded budget and when
this table stops quoting the measurement. The fixture drives every dynamic
value with a hostile value, so the measurement is the worst case for the
document shape, not an accident of short values. `..._BODY_MAX` is the hard
ceiling: a document that would not fit returns `..._OVERFLOW` instead of being
served truncated.

A second, transport-level ceiling applies to every response: the fixture
emits a response as one TLS record through a single `tcp_write()`, and lwIP
accepts that write only when the whole record fits in the TCP send buffer.
The ITEM-384 rendered documents therefore raise the fixture `TCP_SND_BUF` from
the two-MSS default to eight MSS in `kernel/include/lwipopts.h`; without it a
document larger than 2 MSS can never be transmitted and the connection stalls
instead of failing. Any future page or API response that grows past the
record budget must raise that buffer deliberately rather than silently
truncate.

| page | bytes | budget |
| --- | --- | --- |
| login | 2461 | 2461 |
| dashboard | 2759 | 2759 |
| network | 2922 | 2922 |
| services | 2544 | 2544 |
| storage | 2240 | 2240 |
| system | 2917 | 2917 |
| security | 3209 | 3209 |
| diagnostics | 2244 | 2244 |
| updates | 2643 | 2643 |
| recovery | 2653 | 2653 |
| routing | 3532 | 3532 |
| account | 3073 | 3073 |
| certificates | 3123 | 3123 |
| help | 3650 | 3650 |
| licenses | 2736 | 2736 |
| acme | 2797 | 2797 |
| gate | 3648 | 3648 |

ITEM-509 increment 11 rewired the read-side pages to the string catalog and
made the budget measurement render each page in **both** languages, recording
the longer document: the Russian prose is longer than the English on every
read page, so the English-only numbers no longer covered the document a machine
with `v1:system.language=ru` actually serves. The budget is the maximum of the
two renderings and stays below `..._BODY_MAX` (3584) on every page; nothing was
raised above the hard ceiling.

ITEM-509 increment 12 rewired the forms, the two-phase confirmations, the
operator certificate import, the licenses page and the sign-in document to the
same catalog. The English documents are byte-for-byte what they were, so the
QEMU markers and the static `/` shell are untouched; the Russian column is the
longer one, and the rows above are the measured `max(en, ru)`. The login row is
the largest move (1611 -> 2461) because the measurement now covers the
signed-in document the panel actually serves - the sign-out form, the
`session N, principal M` pair, a notice and the throttle line - instead of the
anonymous shell alone; the old number under-measured those shapes in English
already. The pages that carry a form (network, services, updates, recovery,
system, security, routing, account, certificates) and the licenses and acme
pages move by their Russian prose. `..._BODY_MAX`, `..._BUDGET_MAX` and
`HTTP_RESPONSE_BODY_MAX` stay at 3584: the worst page is still the Dashboard at
2590 bytes, so no static buffer grows.

ITEM-509 increment 8 added the `help` row and the `Help` navigation link, so
every section page and the licenses page grew by the 31 bytes of that link (the
static sign-in shell's index by the five bytes of the name) and was re-baselined
together with the header constants. The Help section is documented below. The
same increment raised `..._BODY_MAX` from 2048 to 3328: the help index and the
longest topic page (the Russian `storage` body) measure 3122 and 3179 bytes, so
the old ceiling truncated them with `..._OVERFLOW`. The raise costs 4 x 1280
bytes of static RAM in `struct appliance_management_panel` (the login, section,
licenses and ACME page buffers) and stays inside the transport budget the
paragraph above names - a ~3.2 KiB document is one TLS record well below the
eight-MSS `TCP_SND_BUF`.

ITEM-509 increment 10 made the navigation, the section heading, the footer and
the `<html lang>` attribute follow the machine's language. The English documents
are byte-for-byte what they were (the catalog's English values are the old
literals), so every other row in the table is unchanged; the worst Help page is
the Russian `storage` topic, and it now carries the Russian navigation as well,
so its row moves 3179 -> 3350. `..._BODY_MAX` and `HTTP_RESPONSE_BODY_MAX` move
3328 -> 3584 for the same reason (4 x 256 bytes more static RAM in the panel;
the assembled wire buffer is unchanged). The per-page budgets stay the measured
English worst cases until increment 11 translates the page bodies and re-measures
`max(en, ru)` page by page; the ceiling already covers the navigation growth. A
page whose catalog key has no value in the requested language is refused with
`..._MISSING_TRANSLATION` and answered as a named 503, never published half
translated.

ITEM-509 increment 25 translated the Help index sentence and the way back, so
the Russian Help pages carry Russian prose where they carried the English
literals. The English documents are byte-for-byte what they were (the two halves
of the index sentence join around the same `<code>help</code>` markup); the
worst page is still the Russian `storage` topic, and its row moves
3460 -> 3481 for the longer Russian label.

ITEM-513 added the nav's one-click language switch, so every panel page except
the signed-out sign-in shell grew by the same 169-byte form (the two `EN`/`RU`
submit buttons and their `style="display:inline"`), and the table above is the
re-measured `max(en, ru)` for every row: the login row is unchanged because the
switch is not offered without a session, and the worst page - the Russian Help
topic - moves 3481 -> 3650. The ceiling moves with it: `..._BODY_MAX` and
`HTTP_RESPONSE_BODY_MAX` 3584 -> 3840, `HTTP_RESPONSE_BUFFER_MAX` 4256 ->
4512; the four static panel page buffers and the per-connection response buffer
grow by 256 bytes each, and the document is still one TLS record below the
eight-MSS send buffer. The System page's stage form is split into one form per
field in the same change, because the stage parses exactly one field per body:
the body a browser submits is now the body the page draws.

ITEM-515 finished that rule for every panel form. The Network page drew its
five controls bare and its stage refused `mode=dhcp` whenever the four optional
inputs were present at all, which is exactly what a browser sends, so the
operator could not change the address and could not return to DHCP from the
panel; the fields now carry `<label>` text (the prefix label says the number is
the bit count, without a slash) and the stage treats an empty optional input as
an absent value. Every network refusal is a name in the refusal document -
`NETWORK_NO_PREFIX`, `NETWORK_EXTRA_FIELD`, `NETWORK_ADDRESS_INVALID` and the
rest - instead of one generic `REFUSED`, as the installer's own refusals
already are. The rule is held by `tests/appliance-panel-forms-host.c`, which
renders each page, derives every form's body from the controls actually drawn
(a select submits its selected option, a text input its value or the empty
string, a checkbox only when checked, a clicked button only when clicked) and
drives the parser the serving path drives. The five labels moved the Network
row 2599 -> 2754.

ITEM-382 added the `licenses` row and the `Licenses` navigation link, so every
section page grew by the 45 bytes of that link and was re-baselined together
with the header constants. The page itself is documented in
`docs/hfoc-appliance-license-surface.md`.

ITEM-508 increment 34 added the `acme` row: `/panel/acme` is a page of its own,
not a management section, because the Dashboard that carries the read-only ACME
block was measured at the then-2048-byte cap and the control forms do not
fit there.
The page shows the durable enable/disable/request setting from the ACME control
record, offers the three stage forms and the confirmation form, and names its
reason instead of disappearing when no record is published. It carries no
certificate, key or ACME token; the only token on it is the mutation plane's
single-use confirmation field, exactly as on the other configuring pages. The
Dashboard's ACME block links to it, which is the dashboard row's 1911 -> 1942
move. ITEM-508 increment 37 added the order path's own `STATE/STEP` to that
read-only block (`RUNNING/ORDER`, `DONE/CERTIFICATE`, `FAILED/CHALLENGE`), so
the operator sees whether the issuance they requested is running, finished or
refused by name; the row moved 1942 -> 1973 with it.

ITEM-506 increment 6 added the `Account` section and its navigation link, so
every section page and the licenses page grew by the 37 bytes of that link and
was re-baselined together with the header constants. The licenses page was
within a few bytes of the then-2048-byte ceiling; its trailing sentence naming the
internal `LICENSE_PAYLOAD` acceptance stage was dropped to keep the page inside
the ceiling (the stage itself is unchanged and still named by the build
acceptance matrix and its documents). The Account page carries the stored
login, the four-field credential-change form and the confirmation form, and
never renders a password.

ITEM-499 removed the duplicated `/panel` prefix from every menu link, so each
section page and the licenses page shrank by the seven bytes of one redundant
`/panel` occurrence per non-Dashboard link (`-56` bytes for the eight sections
after Dashboard, `-59` for licenses, which carries the same eight plus its own
link) and was re-baselined together with the header constants again. The
recorded budget is the measurement, so the next byte of growth fails the host
test until it is reviewed and recorded.

ITEM-507 increment 5 added the `Certificates` section and its navigation link,
so every section page and the licenses page grew by the 46 bytes of that link
and was re-baselined together with the header constants. The page lists the
active table's summary (how many names, the default declaration and the source
it was read from) next to the saved table's bounded `name (bytes)` list, and
carries two compact mutation forms - import, and delete plus the token
confirmation - because the then-2048-byte ceiling left no room for one form per
action. The declaration action stays on the scriptable API target. The
licenses page was again within a few bytes of the ceiling; the host test's
hostile-value rendering is what sets its budget, and the fixed markup was
trimmed (inter-tag newlines and the redundant colon after `Reason`) to keep it
inside. The page never renders a key or a certificate body.

ITEM-649 keeps the name-certificate form's refusal named.  When the ITEM-503
import gate refuses because the wall clock is not yet validated, the panel and
the API target answer `{"refused":"BAD_CLOCK"}` instead of the generic
`REFUSED`; the candidate, including the private key, is zeroed before that
answer.  Every other refusal keeps its existing answer, so no operator sees a
different result for an unchanged cause.

ITEM-657 fixes the confirmation step for that page.  A staged
`CERTIFICATE_NAME` mutation (import, delete or default) answered 303 back to
`/panel/certificates`, but the serving path's pending list omitted
`CERTIFICATE_NAME`, so the page never rendered the token and the operator had
no way to confirm the change; the saved table stayed empty.  The list now lives
in one helper,
`appliance_management_authorization_mutation_has_panel_confirmation()`, which
the serving path uses and the panel host test checks for every mutation with a
page.  The page and the confirmation gate therefore cannot drift apart again.

ITEM-505 increment 2 gave the generic sections a live view on a real appliance
(the QEMU fixture was the only publisher before it), so the host test now
measures every generic section twice - with the hostile short fixture values and
with the worst case the production view can publish (`appliance_production_
panel_view.h` bounds the hostname, the MAC, the service name, the detail line
and the recent-log line) - and records the longer rendering. The Dashboard,
Services, Storage, Diagnostics, Updates and Recovery rows above are that
re-baseline. The recent-log field is bounded by the renderer's own per-value
bound (`APPLIANCE_MANAGEMENT_WEB_UI_VALUE_MAX`), so the line the appliance
publishes is rendered whole and is never cut silently; a value that would not fit
is published as `unavailable` instead.

ITEM-505 increment 3 turned the System section into a configuring page: it
carries the active identity, the saved hostname, the source of the saved record
and `applies=reboot` next to the one-field hostname form and the token
confirmation form, so its budget was re-measured with both hostnames at the
longest value the configuration model accepts (63 bytes) and recorded as 1810.

## HTTPS proof

The real TLS 1.3 HTTPS QEMU service verifies the anonymous sign-in document on
`GET /` and `HEAD /`, the section documents on `GET /panel` and
`GET /panel/network` with a bearer session, the `401` denial without one, and
the exact hardened head on every response. All HTTPS scripts that previously
asserted the ITEM-345 shell body or the old `<h1>hfoc</h1>` root body are
updated to assert the rendered panel, its `Content-Length`, and the new head.

## Safety

No plaintext password, bearer token, private key, credential fingerprint,
or other secret material is accepted, rendered, or stored by this module.
The panel renders the confirmation token it is handed and never accepts
credential or destructive-operation input itself. The production kernel and
production ISO contain no fixture-only proof marker or secret-bearing
management panel string; the renderer is compiled into the production kernel,
while the panel dispatch and the wire proof stay in the HTTPS fixture and host
tests.

## Boundaries

- ITEM-344 owns local console presentation.
- ITEM-345 owns the management shell origin, section model, and TLS proof.
- ITEM-346 owns integrated installer/dual-boot/management acceptance.
- ITEM-347 owns documentation and hardware acceptance procedures.
- ITEM-366/367A own the management plane, the mutation plane, and the
  two-phase confirmation token the panel publishes.
- ITEM-375 owns the TLS termination and the fixed response head.
- ITEM-384 (this item) owns rendering, the panel session presentation, the
  hardening headers, escaping, and the asset budget.
- ITEM-385 owns management reachability policy.
- ITEM-445 owns the browser sign-in: the form document, the bounded request
  body, the one session cookie, the sign-out target, and the credential
  boundary the serving path binds to the ITEM-401 enrollment.
- ITEM-499 owns the navigation targets: the menu prints the declared path once,
  the host proof refuses a published target the dispatcher does not serve, and
  the QEMU proof walks the served menu as a remote client.
- ITEM-513 owns the per-browser language: the nav's one-click `EN | RU` form,
  the `hfoc_lang` cookie and its priority over `v1:system.language`, and the
  one-field-per-form System stage forms.

## Non-Goals

- No production browser UI, JavaScript application, or external web
  framework.
- No second credential path: the one remote credential form posts to the
  appliance itself and consults the existing enrollment, throttle and session
  store. There is no password database, no second authentication factor, no
  password recovery flow, and exactly two cookies: the HttpOnly session cookie
  and, since ITEM-513, the per-browser `hfoc_lang` preference (HttpOnly,
  Secure, SameSite=Strict, Path=/, no Max-Age) the nav's one-click switch sets.
  Neither cookie carries a credential beyond the session token the existing
  administrator boundary issued, and the language cookie changes no machine
  state.
- No change to the read-only GET management API responses or `auth=unavailable`
  production state. ITEM-505 adds one deliberate exception: `POST
  /api/v1/network` and its panel form are the two-phase network-configuration
  mutation, while every GET body stays as ITEM-400 published it.
- No change to the fixed HTTP route table, and no route registration.
- No physical-hardware or production-browser acceptance.
- No VERSION or release-number change.

## ITEM-516: страница гейта управления

`/panel/gate` — страница своего собственного права, как `/panel/acme`: она
показывает durable-настройку `v1:security.panel.after-boot` (`OPEN_AFTER_BOOT`
или `CLOSED_AFTER_BOOT`), источник значения (`COMMITTED`/`DEFAULT`), честное
`applies=reboot` и полное предупреждение — потеря физического присутствия,
пароль и блокировка после неудач как единственная защита, находка аудита R01
(wildcard-адрес слушателя) и что это значит на выставленном в Интернет
аппарате.  Единственная форма действия страницы несёт `action=close`:
включение остаётся локальной консольной командой `gate`, а попытка открыть из
панели отвергается по имени `GATE_LOCAL_SECRET_REQUIRED` до выдачи токена и
без записи.  Коммит закрытия идёт обычной двухфазной мутацией (`PANEL_GATE`,
токен, подтверждение, отмена), а страница показывает ожидающий кандидат тем же
блоком подтверждения, что и остальные разделы.  Страница связана со разделом
Security; её бюджет — 3480 байт (худший случай из двух языков), а бюджет
Security после добавления ссылки — 2904.
