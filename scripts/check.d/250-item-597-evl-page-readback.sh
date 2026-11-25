#!/usr/bin/env bash
# ITEM-597: scope and publication wiring; behavioral proof is in host targets.
python3 - <<'PY'
from pathlib import Path

driver = Path('kernel/src/rtl8168.c').read_text()
strict = driver.split('enum rtl8168_status rtl8168_phy_select_page(', 1)[1].split('\n}', 1)[0]
assert 'if (observed != page)' in strict
assert 'RTL8168_PHY_FAILURE_PAGE_MISMATCH' in strict
assert strict.count('rtl8168_phy_write(') == 1
assert strict.count('rtl8168_phy_read(') == 1
evl = driver.split('static enum rtl8168_status rtl8168_evl_select_vendor_page(', 1)[1].split('\n}', 1)[0]
assert evl.count('rtl8168_phy_write(') == 1
assert evl.count('rtl8168_phy_read(') == 1
assert 'return rtl8168_phy_select_page(io, page);' in evl
assert 'delay_us(' not in evl
assert 'observed == 0U' in evl and 'observed != page' in evl
assert 'phy->page_selection.read = io->phy_last;' in evl
assert driver.count('rtl8168_evl_select_vendor_page(') == 2
assert 'rtl8168_evl_select_vendor_page(io, phy, setting->page)' in driver
record = Path('kernel/src/appliance_production_network_phy_record.c').read_text()
assert 'info->page_selection = phy->page_selection;' in record
assert 'policy=EVL_ZERO_UNVERIFIED' in record
assert '{ appliance_production_network_phy_select_line },' in record
assert 'appliance_production_network_phy_select_line(&info, phy,' in Path('kernel/src/kernel.c').read_text()
test = Path('tests/net-phy-journal-host.c').read_text()
assert 'appliance_production_network_phy_select_line,' in test
assert 'test_evl_zero_readback_is_published();' in test
assert 'zero selector diag export passes the operator CRC reader' in test
assert 'test_evl_phy_zero_page_readback();' in Path('tests/rtl8168-host.c').read_text()
print('ITEM-597: strict default, EVL-only zero observation and production export wiring pinned')
PY
