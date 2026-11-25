#!/usr/bin/env bash
# ITEM-596: retain page write/read evidence without changing the PHY protocol.
python3 - <<'PY'
from pathlib import Path

driver = Path('kernel/src/rtl8168.c').read_text()
select = driver.split('enum rtl8168_status rtl8168_phy_select_page(', 1)[1].split('\n}', 1)[0]
assert select.count('rtl8168_phy_write(') == 1, 'page evidence must not add writes'
assert select.count('rtl8168_phy_read(') == 1, 'page evidence must not add reads'
assert 'delay_us(' not in select, 'page evidence must not change delays'
assert select.index('page_write = io->phy_last;') < select.index('rtl8168_phy_read(')
assert 'if (observed != page)' in select, 'page mismatch still refuses'
assert 'io->phy_page_write_valid = false;' in driver, 'new accesses discard stale pairs'
assert 'io->phy_last.payload = payload;' in driver, 'raw payload must be observed'
assert 'io->phy_last.payload_valid = true;' in driver
assert 'state->failure.page_write = io->phy_page_write;' in driver
assert 'state->failure.valid || io->phy_last.reason == RTL8168_PHY_FAILURE_NONE' in driver
record = Path('kernel/src/appliance_production_network_phy_record.c').read_text()
assert '[NET-PHY-PAGE] write_cmd=' in record
assert 'failure->transaction.payload_valid' in record
assert '{ appliance_production_network_phy_page_line },' in record
assert 'appliance_production_network_phy_page_line(&info, phy,' in Path('kernel/src/kernel.c').read_text()
test = Path('tests/net-phy-journal-host.c').read_text()
for proof in (
    'page refusal preserves the preceding write and raw payload',
    'page evidence adds no PHY commands and never bypasses refusal',
    'page pair fits the console and refuses a truncated record',
    'diag export retains the page pair and truthful refusal',
    'successful and failed refreshes retain all first page-pair bytes',
    'write_polls=2 read_payload=0x805F0002',
    'write_polls=2 read_payload=UNREAD',
):
    assert proof in test, proof
for name in ('docs/ARCHITECTURE.md', 'docs/ROADMAP.md', 'docs/rtl8168evl-phy-audit.md'):
    assert 'ITEM-596' in Path(name).read_text(), name
print('ITEM-596: page pair, first failure, export wiring and unchanged bus access pinned')
PY
