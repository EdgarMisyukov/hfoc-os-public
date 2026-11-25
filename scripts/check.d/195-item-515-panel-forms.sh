#!/usr/bin/env bash
# ITEM-515: contracted slice of the HFOC OS check suite. Sourced in pinned order
#           by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The owner's finding (2026-10-10): the network could not be changed from the
# panel.  Three defects, all of them the same class ITEM-513 had closed once
# for the System page: the panel's proofs posted hand-written bodies while a
# browser submits every control the page draws, empty ones included.
#
#   * `mode=dhcp` arrived with the four optional inputs present and empty, and
#     the stage refused any body where they were present at all, so the branch
#     could never run from a browser and the operator could not return to DHCP;
#   * the four fields were drawn bare - nothing named the address, the prefix,
#     the gateway or the DNS, and nothing said the prefix is the number alone;
#   * every refusal was one generic word, so "no prefix", "address not parsed"
#     and "stray field" were indistinguishable, while the installer names each
#     of its own refusals.
#
# The pins below hold the three fixes and the rule they belong to: the panel's
# form grammar is one host-testable module, the drawn body is the accepted
# body for every panel form, a network refusal is a name, and the proofs that
# say so exist and run.
for item515_file in \
    kernel/include/appliance_panel_form.h \
    kernel/src/appliance_panel_form.c \
    kernel/src/appliance_production_serving.c \
    kernel/src/appliance_management_web_ui.c \
    kernel/src/appliance_language.c \
    kernel/include/appliance_management_web_ui.h \
    tests/appliance-panel-forms-host.c \
    tests/appliance-certificate-upload-host.c \
    scripts/test-qemu-production-service-https.sh \
    docs/hfoc-management-web-ui.md \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item515_file ]] || {
        echo "ITEM-515: отсутствует $item515_file" >&2
        exit 1
    }
done

# 1. The form grammar is one module, and the DHCP branch treats the empty
#    inputs a browser always sends as absent values: only a non-empty one is
#    the stray field, because a form that meant static and lost its mode must
#    not silently become a different change.
grep -Fq 'appliance_panel_form_network_parse' \
    kernel/include/appliance_panel_form.h \
    kernel/src/appliance_panel_form.c \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-515: разбор формы сети не вынесен в общий модуль" >&2
    exit 1
}
for item515_empty in \
    '(address_present && address_length != 0U)' \
    '(prefix_present && prefix_length != 0U)' \
    '(gateway_present && gateway_length != 0U)' \
    '(dns_present && dns_length != 0U)'; do
    grep -Fq -- "$item515_empty" kernel/src/appliance_panel_form.c || {
        echo "ITEM-515: DHCP-ветка не игнорирует пустое поле: $item515_empty" >&2
        exit 1
    }
done
if grep -Fq 'if (address_present || prefix_present || gateway_present ||' \
    kernel/src/appliance_panel_form.c; then
    echo "ITEM-515: DHCP-ветка снова отказывает по присутствию поля" >&2
    exit 1
fi

# 2. Every refusal is named like the installer's own, and the name is what the
#    HTTP refusal document quotes.
for item515_name in \
    'return "NETWORK_EMPTY_BODY";' \
    'return "NETWORK_MODE_MISSING";' \
    'return "NETWORK_MODE_UNKNOWN";' \
    'return "NETWORK_EXTRA_FIELD";' \
    'return "NETWORK_NO_ADDRESS";' \
    'return "NETWORK_NO_PREFIX";' \
    'return "NETWORK_NO_GATEWAY";' \
    'return "NETWORK_NO_DNS";' \
    'return "NETWORK_ADDRESS_INVALID";' \
    'return "NETWORK_PREFIX_INVALID";' \
    'return "NETWORK_GATEWAY_INVALID";' \
    'return "NETWORK_DNS_INVALID";'; do
    grep -Fq -- "$item515_name" kernel/src/appliance_panel_form.c || {
        echo "ITEM-515: отказ сети не назван: $item515_name" >&2
        exit 1
    }
done
for item515_serving in \
    'serving->network_refusal = appliance_panel_form_status_string(' \
    'serving->network_refusal = NULL;' \
    'reason = serving->network_refusal != NULL'; do
    grep -Fq -- "$item515_serving" \
        kernel/src/appliance_production_serving.c || {
        echo "ITEM-515: serving не проводит имя отказа: $item515_serving" >&2
        exit 1
    }
done

# 3. The other panel grammars the proof drives are the ones the stages run:
#    the one-field System form and the one-field action form.
for item515_parser in \
    'appliance_panel_form_system_parse' \
    'appliance_panel_form_action_parse'; do
    grep -Fq -- "$item515_parser" \
        kernel/src/appliance_production_serving.c || {
        echo "ITEM-515: стадия не использует общий разбор: $item515_parser" >&2
        exit 1
    }
done
grep -Fq 'appliance_panel_form_system_parse' \
    kernel/src/appliance_panel_form.c || {
    echo "ITEM-515: разбор System потерян" >&2
    exit 1
}

# 4. The drawn Network form names every control, and the prefix label says the
#    number is the bit count alone; the field names the serving path reads are
#    unchanged.
for item515_label in \
    'panel.network.label_mode' \
    'panel.network.label_address' \
    'panel.network.label_prefix' \
    'panel.network.label_gateway' \
    'panel.network.label_dns'; do
    grep -Fq -- "$item515_label" \
        kernel/src/appliance_management_web_ui.c \
        kernel/src/appliance_language.c || {
        echo "ITEM-515: подпись поля сети не нарисована: $item515_label" >&2
        exit 1
    }
done
grep -Fq '"<label>"' kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-515: поля сети снова нарисованы без подписей" >&2
    exit 1
}
for item515_field in \
    'name=\"mode\"' \
    'name=\"address\"' \
    'name=\"prefix\"' \
    'name=\"gateway\"' \
    'name=\"dns\"' \
    'name=\"token\"'; do
    grep -Fq "$item515_field" \
        kernel/src/appliance_management_web_ui.c || {
        echo "ITEM-515: форма сети потеряла поле: $item515_field" >&2
        exit 1
        }
done

# 5. The measured price: the page grew by the five labels and was re-measured.
grep -Fq '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_NETWORK 2922U' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-515: бюджет страницы сети не пере-измерен" >&2
    exit 1
}
grep -Fq '| network | 2922 | 2922 |' docs/hfoc-management-web-ui.md || {
    echo "ITEM-515: документ не называет новый замер сети" >&2
    exit 1
}

# 6. The host proof walks the drawn pages and drives the real parsers; it is
#    wired into the Makefile and the host suite, and the upload proof covers
#    the empty optional textarea the Security page draws.
for item515_host in \
    'static bool build_body(' \
    'static void test_network_drawn_body(void)' \
    'static void test_network_refusal_names(void)' \
    'static void test_system_drawn_bodies(void)' \
    'static void test_account_drawn_body(void)' \
    'static void test_login_drawn_body(void)' \
    'static void test_routing_drawn_body(void)' \
    'static void test_acme_drawn_bodies(void)' \
    'static void test_certificates_drawn_bodies(void)' \
    'strcmp(body, "mode=dhcp&address=&prefix=&gateway=&dns=")'; do
    grep -Fq -- "$item515_host" tests/appliance-panel-forms-host.c || {
        echo "ITEM-515: host-проба не несёт: $item515_host" >&2
        exit 1
    }
done
grep -Fq 'test-appliance-panel-forms-host' Makefile || {
    echo "ITEM-515: host-проба не подключена к сборке" >&2
    exit 1
}
grep -Fq '$(MAKE) test-appliance-panel-forms-host' Makefile || {
    echo "ITEM-515: host-проба не вызвана в наборе" >&2
    exit 1
}
grep -Fq '"&chain=&key="' tests/appliance-certificate-upload-host.c || {
    echo "ITEM-515: проба импорта не подаёт пустую цепочку из формы" >&2
    exit 1
}

# 7. The QEMU proof returns the saved record to DHCP with the drawn browser
#    body, names the missing prefix, and reads the durable record.
for item515_qemu in \
    'dhcp_body = b"mode=dhcp&address=&prefix=&gateway=&dns="' \
    '"refused":"NETWORK_NO_PREFIX"' \
    'NETWORK_DHCP_RECORD=' \
    'source=PANEL mode=DHCP persisted=OK applies=reboot current=STATIC' \
    'TLS-PROOF network panel: the drawn DHCP body and the named '; do
    grep -Fq -- "$item515_qemu" \
        scripts/test-qemu-production-service-https.sh || {
        echo "ITEM-515: QEMU-проба не несёт: $item515_qemu" >&2
        exit 1
    }
done

# 8. The item is recorded where the next window reads it.
for item515_doc in docs/ROADMAP.md docs/ARCHITECTURE.md \
    docs/hfoc-management-web-ui.md; do
    grep -Fq 'ITEM-515' "$item515_doc" || {
        echo "ITEM-515: $item515_doc не называет пункт" >&2
        exit 1
    }
done
# ITEM-516 moved the queue's `## Current item` heading on to its own item, so
# this pin reads ITEM-515's own historical section; the heading shape itself is
# held by `scripts/check.d/52-queue-current-item.sh`.
grep -A 3 '^## Item ITEM-515$' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md |
    grep -Fq 'ITEM-515' || {
    echo "ITEM-515: раздел очереди не называет ITEM-515" >&2
    exit 1
}
