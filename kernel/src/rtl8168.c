#include "rtl8168.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * XID -> chip family.  The values are the 8168-family rows of the Linux
 * r8169 mac_info table, matched as (xid & 0x7cf) == val (or 0x7c8 for the
 * rows that use that mask).  Only the two families this driver distinguishes
 * are listed: the extended-MAC parts (rtl_is_8168evl_up(), RTL_GIGA_MAC_VER_34
 * and later) and the classic parts, whose MAC is only in the IDR registers.
 */
static const struct {
    uint16_t mask;
    uint16_t value;
    enum rtl8168_chip_family family;
    /*
     * ITEM-589: the PHY access this family uses, in the same row as the family
     * itself.  One table answers both questions ("which silicon is this" and
     * "how is its PHY reached"), so a mask the table accepts can never be
     * skipped by a second, hand-written XID list - which is exactly how the
     * owner's 0x2C9 part lost its PHY configuration.
     */
    enum rtl8168_phy_access access;
    /*
     * ITEM-592: the confirmed generation profile.  The family is too coarse to
     * decide a cold start: 0x2C0/0x2C1/0x480/0x481/0x488 are the E/F-era
     * starts, while 0x2C8 (and the owner's 0x2C9, which the same mask matches)
     * is the 8168evl - the reference driver's RTL_GIGA_MAC_VER_34 - with its
     * own EPHY/clock start and its own generation bounds.
     */
    enum rtl8168_profile profile;
} rtl8168_xid_table[] = {
    /* 8168H family (RTL8168H/8111H, the acceptance host's rev 15 part). */
    { 0x7CFU, 0x541U, RTL8168_CHIP_EXTENDED, RTL8168_PHY_ACCESS_OCP,
      RTL8168_PROFILE_8168H },
    /* 8168EP family. */
    { 0x7CFU, 0x54BU, RTL8168_CHIP_EXTENDED, RTL8168_PHY_ACCESS_OCP,
      RTL8168_PROFILE_8168EP },
    { 0x7CFU, 0x54AU, RTL8168_CHIP_EXTENDED, RTL8168_PHY_ACCESS_OCP,
      RTL8168_PROFILE_8168EP },
    { 0x7CFU, 0x502U, RTL8168_CHIP_EXTENDED, RTL8168_PHY_ACCESS_OCP,
      RTL8168_PROFILE_8168EP },
    /* 8168G / 8411b family. */
    { 0x7CFU, 0x5C8U, RTL8168_CHIP_EXTENDED, RTL8168_PHY_ACCESS_OCP,
      RTL8168_PROFILE_8168G },
    { 0x7CFU, 0x509U, RTL8168_CHIP_EXTENDED, RTL8168_PHY_ACCESS_OCP,
      RTL8168_PROFILE_8168G },
    { 0x7CFU, 0x4C0U, RTL8168_CHIP_EXTENDED, RTL8168_PHY_ACCESS_OCP,
      RTL8168_PROFILE_8168G },
    /* 8411 / 8168F family. */
    { 0x7C8U, 0x488U, RTL8168_CHIP_EXTENDED, RTL8168_PHY_ACCESS_MII_PHYAR,
      RTL8168_PROFILE_8168E },
    { 0x7CFU, 0x481U, RTL8168_CHIP_EXTENDED, RTL8168_PHY_ACCESS_MII_PHYAR,
      RTL8168_PROFILE_8168E },
    { 0x7CFU, 0x480U, RTL8168_CHIP_EXTENDED, RTL8168_PHY_ACCESS_MII_PHYAR,
      RTL8168_PROFILE_8168E },
    /* 8168E family: 0x2C8 is the 8168evl generation (the owner's 0x2C9). */
    { 0x7C8U, 0x2C8U, RTL8168_CHIP_EXTENDED, RTL8168_PHY_ACCESS_MII_PHYAR,
      RTL8168_PROFILE_8168EVL },
    { 0x7CFU, 0x2C1U, RTL8168_CHIP_EXTENDED, RTL8168_PHY_ACCESS_MII_PHYAR,
      RTL8168_PROFILE_8168E },
    { 0x7C8U, 0x2C0U, RTL8168_CHIP_EXTENDED, RTL8168_PHY_ACCESS_MII_PHYAR,
      RTL8168_PROFILE_8168E },
    /* 8168D/8168DP family. */
    { 0x7CFU, 0x281U, RTL8168_CHIP_CLASSIC, RTL8168_PHY_ACCESS_NONE,
      RTL8168_PROFILE_CLASSIC },
    { 0x7C8U, 0x280U, RTL8168_CHIP_CLASSIC, RTL8168_PHY_ACCESS_NONE,
      RTL8168_PROFILE_CLASSIC },
    { 0x7CFU, 0x28AU, RTL8168_CHIP_CLASSIC, RTL8168_PHY_ACCESS_NONE,
      RTL8168_PROFILE_CLASSIC },
    { 0x7CFU, 0x28BU, RTL8168_CHIP_CLASSIC, RTL8168_PHY_ACCESS_NONE,
      RTL8168_PROFILE_CLASSIC },
    /* 8168C family. */
    { 0x7CFU, 0x3C9U, RTL8168_CHIP_CLASSIC, RTL8168_PHY_ACCESS_NONE,
      RTL8168_PROFILE_CLASSIC },
    { 0x7CFU, 0x3C8U, RTL8168_CHIP_CLASSIC, RTL8168_PHY_ACCESS_NONE,
      RTL8168_PROFILE_CLASSIC },
    { 0x7C8U, 0x3C8U, RTL8168_CHIP_CLASSIC, RTL8168_PHY_ACCESS_NONE,
      RTL8168_PROFILE_CLASSIC },
    { 0x7CFU, 0x3C0U, RTL8168_CHIP_CLASSIC, RTL8168_PHY_ACCESS_NONE,
      RTL8168_PROFILE_CLASSIC },
    { 0x7CFU, 0x3C2U, RTL8168_CHIP_CLASSIC, RTL8168_PHY_ACCESS_NONE,
      RTL8168_PROFILE_CLASSIC },
    { 0x7CFU, 0x3C3U, RTL8168_CHIP_CLASSIC, RTL8168_PHY_ACCESS_NONE,
      RTL8168_PROFILE_CLASSIC },
    { 0x7C8U, 0x3C0U, RTL8168_CHIP_CLASSIC, RTL8168_PHY_ACCESS_NONE,
      RTL8168_PROFILE_CLASSIC },
    /* 8168B family. */
    { 0x7C8U, 0x380U, RTL8168_CHIP_CLASSIC, RTL8168_PHY_ACCESS_NONE,
      RTL8168_PROFILE_CLASSIC },
    { 0x7C8U, 0x300U, RTL8168_CHIP_CLASSIC, RTL8168_PHY_ACCESS_NONE,
      RTL8168_PROFILE_CLASSIC },
};

static bool rtl8168_read8(
    struct rtl8168_mmio_io *io,
    uint32_t offset,
    uint8_t *value)
{
    if (io == NULL || io->read8 == NULL || value == NULL) {
        return false;
    }
    return io->read8(io->context, offset, value);
}

static bool rtl8168_read16(
    struct rtl8168_mmio_io *io,
    uint32_t offset,
    uint16_t *value)
{
    if (io == NULL || io->read16 == NULL || value == NULL) {
        return false;
    }
    return io->read16(io->context, offset, value);
}

static bool rtl8168_read32(
    struct rtl8168_mmio_io *io,
    uint32_t offset,
    uint32_t *value)
{
    if (io == NULL || io->read32 == NULL || value == NULL) {
        return false;
    }
    return io->read32(io->context, offset, value);
}

static bool rtl8168_write8(
    struct rtl8168_mmio_io *io,
    uint32_t offset,
    uint8_t value)
{
    if (io == NULL || io->write8 == NULL) {
        return false;
    }
    return io->write8(io->context, offset, value);
}

static bool rtl8168_write16(
    struct rtl8168_mmio_io *io,
    uint32_t offset,
    uint16_t value)
{
    if (io == NULL || io->write16 == NULL) {
        return false;
    }
    return io->write16(io->context, offset, value);
}

static bool rtl8168_write32(
    struct rtl8168_mmio_io *io,
    uint32_t offset,
    uint32_t value)
{
    if (io == NULL || io->write32 == NULL) {
        return false;
    }
    return io->write32(io->context, offset, value);
}

static bool rtl8168_mmio_complete(const struct rtl8168_mmio_io *io)
{
    return io != NULL &&
           io->read8 != NULL && io->read16 != NULL && io->read32 != NULL &&
           io->write8 != NULL && io->write16 != NULL &&
           io->write32 != NULL;
}

bool rtl8168_pci_identity_supported(
    const struct pci_device_header *header)
{
    if (header == NULL || !header->present) {
        return false;
    }
    return header->vendor_id == RTL8168_VENDOR_ID &&
           header->device_id == RTL8168_DEVICE_ID;
}

enum rtl8168_status rtl8168_memory_bar(
    const struct pci_device_header *header,
    const struct pci_bar **bar)
{
    uint32_t index;

    if (header == NULL || bar == NULL) {
        return RTL8168_NULL;
    }
    *bar = NULL;
    /*
     * The register file is 256 bytes and lives in the first memory BAR of the
     * function.  The reference driver selects the first IORESOURCE_MEM region
     * the same way (pci_select_bars()/ffs()); on the RTL8111/8168 reference
     * layout that is BAR1, with BAR0 the 256-byte I/O window.
     */
    for (index = 0U; index < PCI_BAR_COUNT; ++index) {
        const struct pci_bar *candidate = &header->bars[index];

        if (!candidate->present || candidate->io_space) {
            continue;
        }
        if (candidate->length < RTL8168_MIN_MMIO_LENGTH) {
            continue;
        }
        *bar = candidate;
        return RTL8168_OK;
    }
    return RTL8168_MISSING_MEMORY_BAR;
}

static enum rtl8168_chip_family rtl8168_xid_family(uint16_t xid)
{
    size_t index;

    for (index = 0U;
         index < sizeof(rtl8168_xid_table) / sizeof(rtl8168_xid_table[0]);
         ++index) {
        if ((xid & rtl8168_xid_table[index].mask) ==
            rtl8168_xid_table[index].value) {
            return rtl8168_xid_table[index].family;
        }
    }
    return RTL8168_CHIP_UNKNOWN;
}

static enum rtl8168_profile rtl8168_xid_profile(uint16_t xid)
{
    size_t index;

    for (index = 0U;
         index < sizeof(rtl8168_xid_table) / sizeof(rtl8168_xid_table[0]);
         ++index) {
        if ((xid & rtl8168_xid_table[index].mask) ==
            rtl8168_xid_table[index].value) {
            return rtl8168_xid_table[index].profile;
        }
    }
    return RTL8168_PROFILE_UNKNOWN;
}

const char *rtl8168_profile_string(enum rtl8168_profile profile)
{
    /*
     * ITEM-592: the reference driver's own names for these generations, so a
     * record and its Linux counterpart can be read side by side.
     */
    switch (profile) {
    case RTL8168_PROFILE_CLASSIC:
        return "RTL8168-classic";
    case RTL8168_PROFILE_8168E:
        return "RTL8168e/8111e";
    case RTL8168_PROFILE_8168EVL:
        return "RTL8168evl/8111evl";
    case RTL8168_PROFILE_8168G:
        return "RTL8168g/8111g";
    case RTL8168_PROFILE_8168H:
        return "RTL8168h/8111h";
    case RTL8168_PROFILE_8168EP:
        return "RTL8168ep/8111ep";
    default:
        return "UNKNOWN";
    }
}

static bool rtl8168_profile_at_least(const struct rtl8168_chip *chip,
                                     enum rtl8168_profile minimum)
{
    return chip != NULL && chip->profile >= minimum &&
           chip->profile != RTL8168_PROFILE_UNKNOWN;
}

bool rtl8168_profile_rxdv_gate(const struct rtl8168_chip *chip)
{
    return rtl8168_profile_at_least(chip, RTL8168_PROFILE_8168G);
}

bool rtl8168_profile_rx_early_off(const struct rtl8168_chip *chip)
{
    return rtl8168_profile_at_least(chip, RTL8168_PROFILE_8168G);
}

bool rtl8168_profile_rx_multi_en(const struct rtl8168_chip *chip)
{
    return rtl8168_profile_at_least(chip, RTL8168_PROFILE_8168E);
}

bool rtl8168_profile_tx_auto_fifo(const struct rtl8168_chip *chip)
{
    return rtl8168_profile_at_least(chip, RTL8168_PROFILE_8168EVL);
}

bool rtl8168_profile_evl_cold_boot(const struct rtl8168_chip *chip)
{
    return chip != NULL && chip->profile == RTL8168_PROFILE_8168EVL;
}

bool rtl8168_profile_phy_gigabit_mii(const struct rtl8168_chip *chip)
{
    return chip != NULL &&
           (chip->profile == RTL8168_PROFILE_8168E ||
            chip->profile == RTL8168_PROFILE_8168EVL);
}

enum rtl8168_status rtl8168_chip_identify(
    struct rtl8168_mmio_io *io,
    struct rtl8168_chip *chip)
{
    uint32_t tx_config;

    if (io == NULL || chip == NULL) {
        return RTL8168_NULL;
    }
    memset(chip, 0, sizeof(*chip));
    if (!rtl8168_mmio_complete(io)) {
        return RTL8168_NULL;
    }
    if (!rtl8168_read32(io, RTL8168_REG_TX_CONFIG, &tx_config)) {
        return RTL8168_IO_ERROR;
    }
    if (tx_config == UINT32_MAX) {
        return RTL8168_IO_ERROR;
    }
    /* rtl8169_get_mac_version(): xid = (TxConfig >> 20) & 0xfcf. */
    chip->xid = (uint16_t)((tx_config >> 20U) & 0xFCFU);
    chip->family = rtl8168_xid_family(chip->xid);
    chip->profile = rtl8168_xid_profile(chip->xid);
    if (chip->family == RTL8168_CHIP_UNKNOWN) {
        /*
         * The register map and the family-specific values below are only
         * proven for the identified families.  An XID outside the table is
         * published as unsupported (the caller still has xid and family for
         * the boot record) instead of being driven with guessed values.
         */
        return RTL8168_UNSUPPORTED_DEVICE;
    }
    return RTL8168_OK;
}

const char *rtl8168_chip_name(const struct rtl8168_chip *chip)
{
    if (chip == NULL) {
        return "UNKNOWN";
    }
    switch (chip->family) {
    case RTL8168_CHIP_EXTENDED:
        return "RTL8168-EXTENDED";
    case RTL8168_CHIP_CLASSIC:
        return "RTL8168-CLASSIC";
    default:
        return "RTL8168-UNKNOWN";
    }
}

/* ITEM-399: the family field of the [PRODUCTION-NET] boot record. */
const char *rtl8168_chip_family_string(enum rtl8168_chip_family family)
{
    switch (family) {
    case RTL8168_CHIP_EXTENDED:
        return "RTL8168-EXTENDED";
    case RTL8168_CHIP_CLASSIC:
        return "RTL8168-CLASSIC";
    default:
        return "UNKNOWN";
    }
}

/*
 * ITEM-399: ` xid=0x<hex> family=<family>`, ready to append to the record.  The
 * XID is the value the driver decoded from TxConfig bits 20..31 (the r8169
 * mac_info key), so a hardware run can state which silicon it drove - and which
 * row of the table it matched - instead of only the PCI function id, which the
 * 8168 family shares across a dozen parts.
 */
size_t rtl8168_chip_render_identity(
    const struct rtl8168_chip *chip,
    char *buffer,
    size_t capacity)
{
    static const char digits[] = "0123456789ABCDEF";
    static const char prefix[] = " xid=0x";
    static const char separator[] = " family=";
    const char *family;
    char hex[4U];
    size_t hex_length = 0U;
    size_t needed;
    size_t index;
    size_t offset = 0U;
    uint16_t value;

    if (chip == NULL || buffer == NULL || capacity == 0U) {
        return 0U;
    }
    family = rtl8168_chip_family_string(chip->family);
    value = chip->xid;
    do {
        hex[hex_length] = digits[value & 0xFU];
        ++hex_length;
        value = (uint16_t)(value >> 4U);
    } while (value != 0U && hex_length < sizeof(hex));
    needed = (sizeof(prefix) - 1U) + hex_length + (sizeof(separator) - 1U) +
             strlen(family);
    if (capacity <= needed) {
        return 0U;
    }
    for (index = 0U; index < sizeof(prefix) - 1U; ++index) {
        buffer[offset] = prefix[index];
        ++offset;
    }
    for (index = hex_length; index > 0U; --index) {
        buffer[offset] = hex[index - 1U];
        ++offset;
    }
    for (index = 0U; index < sizeof(separator) - 1U; ++index) {
        buffer[offset] = separator[index];
        ++offset;
    }
    for (index = 0U; family[index] != '\0'; ++index) {
        buffer[offset] = family[index];
        ++offset;
    }
    buffer[offset] = '\0';
    return offset;
}

bool rtl8168_mac_valid(const uint8_t mac[RTL8168_MAC_LENGTH])
{
    size_t index;
    bool non_zero = false;

    if (mac == NULL) {
        return false;
    }
    for (index = 0U; index < RTL8168_MAC_LENGTH; ++index) {
        if (mac[index] != 0U) {
            non_zero = true;
        }
    }
    if (!non_zero) {
        return false;
    }
    if ((mac[0] & 1U) != 0U) {
        /* Multicast or broadcast: never a station address. */
        return false;
    }
    if (mac[0] == 0xFFU && mac[1] == 0xFFU && mac[2] == 0xFFU &&
        mac[3] == 0xFFU && mac[4] == 0xFFU && mac[5] == 0xFFU) {
        return false;
    }
    return true;
}

static enum rtl8168_status rtl8168_eri_read(
    struct rtl8168_mmio_io *io,
    uint16_t address,
    uint32_t *value)
{
    uint32_t command;
    uint32_t observed;
    uint32_t attempts;

    if (value == NULL) {
        return RTL8168_NULL;
    }
    command = RTL8168_ERIAR_EXGMAC | RTL8168_ERIAR_MASK_1111 |
              ((uint32_t)address & RTL8168_ERIAR_ADDRESS_MASK);
    if (!rtl8168_write32(io, RTL8168_REG_ERI_ADDRESS, command)) {
        return RTL8168_IO_ERROR;
    }
    for (attempts = 0U; attempts < RTL8168_ERI_POLL_LIMIT; ++attempts) {
        if (!rtl8168_read32(io, RTL8168_REG_ERI_ADDRESS, &observed)) {
            return RTL8168_IO_ERROR;
        }
        if ((observed & RTL8168_ERIAR_FLAG) != 0U) {
            break;
        }
    }
    if (attempts == RTL8168_ERI_POLL_LIMIT) {
        return RTL8168_IO_ERROR;
    }
    if (!rtl8168_read32(io, RTL8168_REG_ERI_DATA, value)) {
        return RTL8168_IO_ERROR;
    }
    return RTL8168_OK;
}

/* ITEM-592: the write half of the extended-register window. */
static enum rtl8168_status rtl8168_eri_write(
    struct rtl8168_mmio_io *io,
    uint16_t address,
    uint32_t value)
{
    uint32_t command = RTL8168_ERIAR_WRITE | RTL8168_ERIAR_EXGMAC |
                       RTL8168_ERIAR_MASK_1111 |
                       ((uint32_t)address & RTL8168_ERIAR_ADDRESS_MASK);
    uint32_t observed;
    uint32_t attempts;

    if (!rtl8168_write32(io, RTL8168_REG_ERI_DATA, value) ||
        !rtl8168_write32(io, RTL8168_REG_ERI_ADDRESS, command)) {
        return RTL8168_IO_ERROR;
    }
    for (attempts = 0U; attempts < RTL8168_ERI_POLL_LIMIT; ++attempts) {
        if (!rtl8168_read32(io, RTL8168_REG_ERI_ADDRESS, &observed)) {
            return RTL8168_IO_ERROR;
        }
        if ((observed & RTL8168_ERIAR_FLAG) == 0U) {
            return RTL8168_OK;
        }
    }
    return RTL8168_IO_ERROR;
}

static enum rtl8168_status rtl8168_eri_modify(
    struct rtl8168_mmio_io *io,
    uint16_t address,
    uint32_t clear,
    uint32_t set)
{
    uint32_t value = 0U;

    if (rtl8168_eri_read(io, address, &value) != RTL8168_OK) {
        return RTL8168_IO_ERROR;
    }
    return rtl8168_eri_write(io, address, (value & ~clear) | set);
}

static enum rtl8168_status rtl8168_mac_read_eri(
    struct rtl8168_mmio_io *io,
    uint8_t mac[RTL8168_MAC_LENGTH])
{
    uint32_t low;
    uint32_t high;

    if (rtl8168_eri_read(io, RTL8168_ERI_MAC_LOW, &low) != RTL8168_OK ||
        rtl8168_eri_read(io, RTL8168_ERI_MAC_HIGH, &high) != RTL8168_OK) {
        return RTL8168_IO_ERROR;
    }
    mac[0] = (uint8_t)(low & 0xFFU);
    mac[1] = (uint8_t)((low >> 8U) & 0xFFU);
    mac[2] = (uint8_t)((low >> 16U) & 0xFFU);
    mac[3] = (uint8_t)((low >> 24U) & 0xFFU);
    mac[4] = (uint8_t)(high & 0xFFU);
    mac[5] = (uint8_t)((high >> 8U) & 0xFFU);
    return RTL8168_OK;
}

static enum rtl8168_status rtl8168_mac_read_registers(
    struct rtl8168_mmio_io *io,
    uint8_t mac[RTL8168_MAC_LENGTH])
{
    uint32_t low;
    uint16_t high;

    /*
     * The IDR registers are byte addressable; rtl_read_mac_from_reg() reads
     * MAC0..MAC5 as six bytes.  Reading the two wide registers and splitting
     * them is the same access pattern with two bus reads.
     */
    if (!rtl8168_read32(io, RTL8168_REG_MAC0, &low) ||
        !rtl8168_read16(io, RTL8168_REG_MAC4, &high)) {
        return RTL8168_IO_ERROR;
    }
    if (low == UINT32_MAX && high == UINT16_MAX) {
        return RTL8168_IO_ERROR;
    }
    mac[0] = (uint8_t)(low & 0xFFU);
    mac[1] = (uint8_t)((low >> 8U) & 0xFFU);
    mac[2] = (uint8_t)((low >> 16U) & 0xFFU);
    mac[3] = (uint8_t)((low >> 24U) & 0xFFU);
    mac[4] = (uint8_t)(high & 0xFFU);
    mac[5] = (uint8_t)((high >> 8U) & 0xFFU);
    return RTL8168_OK;
}

enum rtl8168_status rtl8168_mac_read(
    struct rtl8168_mmio_io *io,
    const struct rtl8168_chip *chip,
    uint8_t mac[RTL8168_MAC_LENGTH])
{
    uint8_t candidate[RTL8168_MAC_LENGTH];
    enum rtl8168_status status;

    if (io == NULL || mac == NULL) {
        return RTL8168_NULL;
    }
    if (!rtl8168_mmio_complete(io)) {
        return RTL8168_NULL;
    }
    memset(mac, 0, RTL8168_MAC_LENGTH);

    /*
     * rtl_read_mac_address(): on the extended-MAC parts the hardware mirrors
     * the EEPROM-loaded station address in the extended registers 0xe0/0xe4,
     * and the IDR registers are the fallback for every family.  A device whose
     * station address cannot be read is reported, never invented: the
     * reference driver falls back to a random address there, this driver
     * refuses to bring the interface up instead.
     */
    if (chip != NULL && chip->family == RTL8168_CHIP_EXTENDED) {
        memset(candidate, 0, sizeof(candidate));
        status = rtl8168_mac_read_eri(io, candidate);
        if (status == RTL8168_OK && rtl8168_mac_valid(candidate)) {
            memcpy(mac, candidate, RTL8168_MAC_LENGTH);
            return RTL8168_OK;
        }
        if (status != RTL8168_OK && status != RTL8168_IO_ERROR) {
            return status;
        }
    }

    memset(candidate, 0, sizeof(candidate));
    status = rtl8168_mac_read_registers(io, candidate);
    if (status != RTL8168_OK) {
        return status;
    }
    if (!rtl8168_mac_valid(candidate)) {
        return RTL8168_INVALID_MAC;
    }
    memcpy(mac, candidate, RTL8168_MAC_LENGTH);
    return RTL8168_OK;
}

bool rtl8168_dma_address_supported(uint64_t physical)
{
    /*
     * The descriptor address field is 64 bit, but the driver programs no
     * PCI DAC enable and the reference driver does not either
     * (CPCMD_MASK clears PCIDAC), so every address the device is asked to
     * touch must stay inside the 32-bit DMA window.  A region above it is
     * refused instead of handing the part an address it may truncate.
     */
    return physical != 0U && physical < RTL8168_DMA_ADDRESS_LIMIT;
}

enum rtl8168_status rtl8168_interrupts_mask(struct rtl8168_mmio_io *io)
{
    if (io == NULL) {
        return RTL8168_NULL;
    }
    /*
     * This driver is poll driven from the ordinary-boot event loop and IRQ
     * routing is out of scope, so the device must never assert its interrupt
     * line: mask every event and acknowledge whatever is pending.
     */
    if (!rtl8168_write16(io, RTL8168_REG_INTR_MASK,
                         RTL8168_INTR_MASK_NONE)) {
        return RTL8168_IO_ERROR;
    }
    if (!rtl8168_write16(io, RTL8168_REG_INTR_STATUS,
                         RTL8168_INTR_STATUS_CLEAR)) {
        return RTL8168_IO_ERROR;
    }
    return RTL8168_OK;
}

/*
 * ITEM-645: acknowledge the events the poll pass observed.
 *
 * The bring-up acknowledges IntrStatus once; a poll-driven driver that never
 * acknowledges it again leaves a latched `RxOverflow`/`RxFIFOOver` set.  The
 * 2026-11-22 Toshiba capture is exactly that stopped shape: every receive
 * descriptor owned by the NIC and `IntrStatus` `0xA5 -> 0xF5`.  Linux's
 * interrupt handler writes the whole observed word back each time it runs
 * (write-one-to-clear), and the 2026-11 fix for the same shape clears
 * `RxOverflow` in `rtl8169_poll()` once the ring has room; this helper is the
 * poll-driven equivalent.
 *
 * Ordering matters.  The caller drains and re-posts the ring first; only a
 * pass that ended on a descriptor the NIC did not write has proven a free
 * descriptor exists, and only then is the receive-stopped pair cleared.  A
 * busy ring keeps it latched, so this cannot re-arm a receiver with nowhere
 * to write.  The write is one write-one-to-clear word carrying exactly the
 * bits the read observed: a bit that arrives between the read and the write
 * is not included and stays pending for the next pass.
 */
enum rtl8168_status rtl8168_intr_ack_events(
    struct rtl8168_mmio_io *io,
    bool descriptors_available,
    uint16_t *observed,
    uint16_t *acknowledged)
{
    uint16_t status;
    uint16_t ack;

    if (observed != NULL) {
        *observed = 0U;
    }
    if (acknowledged != NULL) {
        *acknowledged = 0U;
    }
    if (io == NULL) {
        return RTL8168_NULL;
    }
    if (!rtl8168_read16(io, RTL8168_REG_INTR_STATUS, &status)) {
        return RTL8168_IO_ERROR;
    }
    if (observed != NULL) {
        *observed = status;
    }
    /*
     * An all-ones status is what a dead bus reads, not sixteen events: the
     * part implements only the low bits.  Never write that back as an
     * acknowledge.
     */
    if (status == RTL8168_INTR_STATUS_CLEAR) {
        return RTL8168_IO_ERROR;
    }
    ack = status;
    if (!descriptors_available) {
        ack &= (uint16_t)~RTL8168_INTR_RX_STOPPED;
    }
    if (ack == 0U) {
        return RTL8168_OK;
    }
    if (!rtl8168_write16(io, RTL8168_REG_INTR_STATUS, ack)) {
        return RTL8168_IO_ERROR;
    }
    if (acknowledged != NULL) {
        *acknowledged = ack;
    }
    return RTL8168_OK;
}

enum rtl8168_status rtl8168_reset(struct rtl8168_mmio_io *io)
{
    uint32_t attempts;
    uint8_t command;

    if (io == NULL) {
        return RTL8168_NULL;
    }
    /* rtl_hw_reset(): ChipCmd = CmdReset, then wait for the bit to clear. */
    if (!rtl8168_write8(io, RTL8168_REG_CHIP_CMD,
                        (uint8_t)RTL8168_CHIP_CMD_RESET)) {
        return RTL8168_IO_ERROR;
    }
    for (attempts = 0U; attempts < RTL8168_RESET_POLL_LIMIT; ++attempts) {
        if (!rtl8168_read8(io, RTL8168_REG_CHIP_CMD, &command)) {
            return RTL8168_IO_ERROR;
        }
        if ((command & RTL8168_CHIP_CMD_RESET) == 0U) {
            return RTL8168_OK;
        }
    }
    return RTL8168_RESET_TIMEOUT;
}

static enum rtl8168_status rtl8168_rx_config_value(
    const struct rtl8168_chip *chip,
    uint32_t *value)
{
    uint32_t rx_config;

    if (value == NULL) {
        return RTL8168_NULL;
    }
    /*
     * rtl_init_rxcfg() for the 8168G..8168EP range (RTL_GIGA_MAC_VER_40..53)
     * is RX128_INT_EN | RX_MULTI_EN | RX_DMA_BURST | RX_EARLY_OFF; the
     * classic 8168 parts use RX128_INT_EN | RX_DMA_BURST.  rtl_set_rx_mode()
     * then replaces the accept field, and
     * rtl_set_rx_config_features() clears the error/runt accept bits first;
     * this driver accepts broadcast and its own station address only, and
     * rejects error frames and runts.
     */
    /*
     * ITEM-592: the reference driver's `rtl_init_rxcfg()` answers by MAC
     * version, not by family: RX_MULTI_EN belongs to the E generation and
     * later, RX_EARLY_OFF only to the G/H/EP ones.  The 8168evl (the owner's
     * part) gets RX_MULTI_EN without RX_EARLY_OFF.
     */
    rx_config = RTL8168_RX128_INT_EN | RTL8168_RX_DMA_BURST;
    if (rtl8168_profile_rx_multi_en(chip)) {
        rx_config |= RTL8168_RX_MULTI_EN;
    }
    if (rtl8168_profile_rx_early_off(chip)) {
        rx_config |= RTL8168_RX_EARLY_OFF;
    }
    rx_config &= ~RTL8168_RX_ACCEPT_ERROR_MASK;
    rx_config &= ~RTL8168_RX_ACCEPT_OK_MASK;
    rx_config |= RTL8168_RX_ACCEPT_BROADCAST |
                 RTL8168_RX_ACCEPT_MY_PHYSICAL;
    *value = rx_config;
    return RTL8168_OK;
}

/*
 * TxConfig.  rtl_set_tx_config_registers() writes DMA burst 7 ("unlimited")
 * and the shortest inter-frame gap, plus TXCFG_AUTO_FIFO for the 8168evl and
 * later parts; the RTL8168B register datasheet calls bits 7-0 reserved, and
 * Realtek's own vendor driver does not set the bit for the 8168H.  This
 * driver follows the Linux reference driver, which is the source of every
 * other value here, and the disagreement is recorded rather than hidden: a
 * hardware run that shows a transmit FIFO problem should try clearing bit 7.
 */
static uint32_t rtl8168_tx_config_value(const struct rtl8168_chip *chip)
{
    uint32_t tx_config = RTL8168_TX_DMA_BURST |
                         RTL8168_TX_INTER_FRAME_GAP;

    /* ITEM-592: TXCFG_AUTO_FIFO is the evl-and-later ("8168evl_up") setting,
       not an E-generation one. */
    if (rtl8168_profile_tx_auto_fifo(chip)) {
        tx_config |= RTL8168_TX_CONFIG_AUTO_FIFO;
    }
    return tx_config;
}

static enum rtl8168_status rtl8168_program_mac(
    struct rtl8168_mmio_io *io,
    const uint8_t mac[RTL8168_MAC_LENGTH])
{
    const uint32_t low = (uint32_t)mac[0] |
                         ((uint32_t)mac[1] << 8U) |
                         ((uint32_t)mac[2] << 16U) |
                         ((uint32_t)mac[3] << 24U);
    const uint32_t high = (uint32_t)mac[4] | ((uint32_t)mac[5] << 8U);
    uint8_t commit;

    /* rtl_rar_set(): MAC4 first, commit, then MAC0, commit. */
    if (!rtl8168_write32(io, RTL8168_REG_MAC4, high)) {
        return RTL8168_IO_ERROR;
    }
    if (!rtl8168_read8(io, RTL8168_REG_CHIP_CMD, &commit)) {
        return RTL8168_IO_ERROR;
    }
    if (!rtl8168_write32(io, RTL8168_REG_MAC0, low)) {
        return RTL8168_IO_ERROR;
    }
    if (!rtl8168_read8(io, RTL8168_REG_CHIP_CMD, &commit)) {
        return RTL8168_IO_ERROR;
    }
    return RTL8168_OK;
}

/*
 * ITEM-592: the 8168evl EPHY table.
 *
 * The integrated PHY's analog/clock block is programmed through EPHYAR before
 * the MAC is enabled.  The four (register, clear, set) rows are the reference
 * driver's `e_info_8168e_2` table for RTL_GIGA_MAC_VER_34 - hardware facts
 * about this generation, written here as data, not as its code.  A vendor
 * blob is not involved and the rows are applied read-modify-write, as the
 * reference driver's `rtl_ephy_init()` does.
 */
struct rtl8168_ephy_step {
    uint8_t reg;
    uint16_t clear;
    uint16_t set;
};

static const struct rtl8168_ephy_step rtl8168_evl_ephy_steps[] = {
    { 0x09U, 0x0000U, 0x0080U },
    { 0x19U, 0x0000U, 0x0224U },
    { 0x00U, 0x0000U, 0x0004U },
    { 0x0CU, 0x3DF0U, 0x0200U },
};

static enum rtl8168_status rtl8168_evl_ephy_stage(
    struct rtl8168_mmio_io *io)
{
    size_t index;

    for (index = 0U;
         index < sizeof(rtl8168_evl_ephy_steps) /
                     sizeof(rtl8168_evl_ephy_steps[0]);
         ++index) {
        const struct rtl8168_ephy_step *step = &rtl8168_evl_ephy_steps[index];
        uint16_t value = 0U;

        if (rtl8168_ephy_read(io, step->reg, &value) != RTL8168_OK) {
            return RTL8168_IO_ERROR;
        }
        value = (uint16_t)((value & (uint16_t)~step->clear) | step->set);
        if (rtl8168_ephy_write(io, step->reg, value) != RTL8168_OK) {
            return RTL8168_IO_ERROR;
        }
    }
    return RTL8168_OK;
}

/*
 * ITEM-592: the 8168evl clock/FIFO/MCU/EEE programming.
 *
 * The reference driver's `rtl_hw_start_8168e_2()` runs this before the MAC is
 * enabled: the extended-register block (FIFO sizes, packet-filter reset, EEE
 * LED and advertisement, exit-L1 events, the two fixed words), then the MCU's
 * out-of-band bit clear, the DLL/PFM clock enable, the PWM block and the SPI
 * clock.  Bit 19 of MISC - the receive data-valid gate - is deliberately NOT
 * part of this stage: that bit belongs to the G/H/EP starts, and writing it on
 * the 8168evl was the generation-boundary defect this item fixes.
 */
static enum rtl8168_status rtl8168_evl_clock_stage(
    struct rtl8168_mmio_io *io)
{
    uint8_t value = 0U;
    uint32_t misc = 0U;

    if (rtl8168_eri_write(io, 0xC0U, 0U) != RTL8168_OK ||
        rtl8168_eri_write(io, 0xB8U, 0U) != RTL8168_OK) {
        return RTL8168_IO_ERROR;
    }
    /* Static/dynamic FIFO split: the reference driver's rtl_set_fifo_size(). */
    if (rtl8168_eri_write(io, 0xC8U, (0x10U << 16U) | 0x02U) != RTL8168_OK ||
        rtl8168_eri_write(io, 0xE8U, (0x10U << 16U) | 0x06U) != RTL8168_OK) {
        return RTL8168_IO_ERROR;
    }
    if (rtl8168_eri_modify(io, 0x1D0U, 0U, 1U << 1U) != RTL8168_OK) {
        return RTL8168_IO_ERROR;
    }
    /* Reset the packet filter: clear then set its own bit. */
    if (rtl8168_eri_modify(io, 0xDCU, 1U, 0U) != RTL8168_OK ||
        rtl8168_eri_modify(io, 0xDCU, 0U, 1U) != RTL8168_OK) {
        return RTL8168_IO_ERROR;
    }
    if (rtl8168_eri_modify(io, 0x1B0U, 0U, 1U << 4U) != RTL8168_OK ||
        rtl8168_eri_write(io, 0xCCU, 0x00000050U) != RTL8168_OK ||
        rtl8168_eri_write(io, 0xD0U, 0x07FF0060U) != RTL8168_OK) {
        return RTL8168_IO_ERROR;
    }
    /* MCU: the part is not in out-of-band (firmware/WOL) mode any more. */
    if (!rtl8168_read8(io, RTL8168_REG_MCU, &value)) {
        return RTL8168_IO_ERROR;
    }
    if (!rtl8168_write8(io, RTL8168_REG_MCU,
                        (uint8_t)(value &
                                  (0xFFU ^ RTL8168_MCU_NOW_IS_OOB)))) {
        return RTL8168_IO_ERROR;
    }
    /* EEE: the LED frequency and the MAC-side advertisement bits. */
    if (!rtl8168_read8(io, RTL8168_REG_EEE_LED, &value)) {
        return RTL8168_IO_ERROR;
    }
    if (!rtl8168_write8(io, RTL8168_REG_EEE_LED,
                        (uint8_t)(value & (0xFFU ^ RTL8168_EEE_LED_MASK)))) {
        return RTL8168_IO_ERROR;
    }
    if (rtl8168_eri_modify(io, 0x1B0U, 0U, 0x0003U) != RTL8168_OK) {
        return RTL8168_IO_ERROR;
    }
    /* Clock: PFM on, the PWM block on, the SPI clock off. */
    if (!rtl8168_read8(io, RTL8168_REG_DLLPR, &value)) {
        return RTL8168_IO_ERROR;
    }
    if (!rtl8168_write8(io, RTL8168_REG_DLLPR,
                        (uint8_t)(value | RTL8168_DLLPR_PFM_EN))) {
        return RTL8168_IO_ERROR;
    }
    if (!rtl8168_read32(io, RTL8168_REG_MISC, &misc)) {
        return RTL8168_IO_ERROR;
    }
    if (!rtl8168_write32(io, RTL8168_REG_MISC, misc | RTL8168_MISC_PWM_EN)) {
        return RTL8168_IO_ERROR;
    }
    if (!rtl8168_read8(io, RTL8168_REG_CONFIG5, &value)) {
        return RTL8168_IO_ERROR;
    }
    if (!rtl8168_write8(io, RTL8168_REG_CONFIG5,
                        (uint8_t)(value &
                                  (uint8_t)~RTL8168_CONFIG5_SPI_EN))) {
        return RTL8168_IO_ERROR;
    }
    /* Exit-L1 events: the reference driver's rtl_enable_exit_l1() row. */
    return rtl8168_eri_modify(io, 0xD4U, 0U, 0x00001F00U);
}

/*
 * ITEM-592: the ordinary MAC start, split out of `rtl8168_bring_up()` so the
 * cold-boot composition can run it as one stage after the EPHY and clock
 * stages instead of resetting the part a second time.
 */
static enum rtl8168_status rtl8168_mac_stage(
    struct rtl8168_mmio_io *io,
    const struct rtl8168_chip *chip,
    const struct rtl8168_bring_up *configuration,
    const uint8_t mac[RTL8168_MAC_LENGTH])
{
    uint32_t rx_config;
    uint32_t tx_config;
    uint16_t cplus;
    uint16_t rx_max_size;
    uint8_t commit;
    uint32_t index;

    if (rtl8168_interrupts_mask(io) != RTL8168_OK) {
        return RTL8168_IO_ERROR;
    }

    /* rtl_hw_start(): unlock the config registers for the setup below. */
    if (!rtl8168_write8(io, RTL8168_REG_CFG9346,
                        (uint8_t)RTL8168_CFG9346_UNLOCK)) {
        return RTL8168_IO_ERROR;
    }
    /*
     * ITEM-592: `rtl_hw_start_8168()` programs the early transmit size on the
     * evl generation and later, and the plain maximum on the older parts.
     */
    if (!rtl8168_write8(io, RTL8168_REG_MAX_TX_PACKET_SIZE,
                        (uint8_t)(rtl8168_profile_evl_cold_boot(chip) ||
                                          rtl8168_profile_rx_early_off(chip)
                                      ? RTL8168_MAX_TX_PACKET_SIZE_EARLY
                                      : 0xFFU))) {
        return RTL8168_IO_ERROR;
    }
    /* Interrupt coalescing is disabled entirely; the driver polls. */
    if (!rtl8168_write16(io, RTL8168_REG_INTR_MITIGATE, 0U)) {
        return RTL8168_IO_ERROR;
    }

    /*
     * ITEM-435: open the receive data-valid gate before the receiver exists.
     *
     * The 8168G-and-later parts gate the receive data path behind bit 19 of
     * MISC, and mainline r8169 opens it for exactly this part's MAC version:
     * `rtl_hw_start_8168h_1()` calls `rtl_disable_rxdvgate()`, which writes
     * `MISC & ~RXDV_GATED_EN` (`RXDV_GATED_EN = BIT(19)`), from MAC version 40
     * on.  A part whose gate stays closed answers every register, completes
     * transmit descriptors and never writes a receive descriptor - no frame,
     * no receive error, no interrupt - which is the 2026-09-17 hardware run's
     * record exactly: `tx_recl` advancing with `rx_ok=0 rx_err=0` for fifteen
     * seconds.  The gate is the extended family's; a classic part is left
     * alone, because this driver only drives the register map it proved.
     *
     * The read-modify-write keeps every other MISC bit the part came up with,
     * as the reference driver's own helper does.
     */
    /*
     * ITEM-592: and it is the G/H/EP parts that own that bit.  The 8168evl
     * start (`rtl_hw_start_8168e_2`) never touches MISC's bit 19; clearing it
     * on that generation wrote a bit whose meaning this driver had not proved,
     * which is exactly the kind of generation-boundary mistake the owner's
     * board cannot afford.
     */
    if (rtl8168_profile_rxdv_gate(chip)) {
        uint32_t misc;

        if (!rtl8168_read32(io, RTL8168_REG_MISC, &misc)) {
            return RTL8168_IO_ERROR;
        }
        if ((misc & RTL8168_MISC_RXDV_GATED_EN) != 0U &&
            !rtl8168_write32(io, RTL8168_REG_MISC,
                             misc & ~RTL8168_MISC_RXDV_GATED_EN)) {
            return RTL8168_IO_ERROR;
        }
    }

    /*
     * CPlusCmd: keep the part's own value inside CPCMD_MASK and clear the
     * receive VLAN tag and receive checksum offload bits, which this driver
     * never uses; the reference driver writes the same masked value.
     */
    if (!rtl8168_read16(io, RTL8168_REG_CPLUS_CMD, &cplus)) {
        return RTL8168_IO_ERROR;
    }
    cplus &= (uint16_t)(0x2000U | 0x0040U | 0x0020U | 0x0003U);
    cplus &= (uint16_t)~(0x0040U | 0x0020U);
    if (!rtl8168_write16(io, RTL8168_REG_CPLUS_CMD, cplus)) {
        return RTL8168_IO_ERROR;
    }

    {
        uint8_t config3;

        if (!rtl8168_read8(io, RTL8168_REG_CONFIG3, &config3)) {
            return RTL8168_IO_ERROR;
        }
        if (!rtl8168_write8(io, RTL8168_REG_CONFIG3,
                            (uint8_t)(config3 &
                                      (uint8_t)~RTL8168_CONFIG3_READY_TO_L23))) {
            return RTL8168_IO_ERROR;
        }
    }

    if (rtl8168_program_mac(io, mac) != RTL8168_OK) {
        return RTL8168_IO_ERROR;
    }

    rx_max_size = (uint16_t)configuration->rx_buffer_capacity;
    if (!rtl8168_write16(io, RTL8168_REG_RX_MAX_SIZE, rx_max_size)) {
        return RTL8168_IO_ERROR;
    }

    /* Descriptor start registers: high half first (rtl_set_rx_tx_desc_registers). */
    if (!rtl8168_write32(
            io, RTL8168_REG_TX_DESC_HIGH,
            (uint32_t)(configuration->tx_descriptor_physical >> 32U)) ||
        !rtl8168_write32(
            io, RTL8168_REG_TX_DESC_LOW,
            (uint32_t)(configuration->tx_descriptor_physical &
                       UINT32_C(0xFFFFFFFF))) ||
        !rtl8168_write32(
            io, RTL8168_REG_RX_DESC_HIGH,
            (uint32_t)(configuration->rx_descriptor_physical >> 32U)) ||
        !rtl8168_write32(
            io, RTL8168_REG_RX_DESC_LOW,
            (uint32_t)(configuration->rx_descriptor_physical &
                       UINT32_C(0xFFFFFFFF)))) {
        return RTL8168_IO_ERROR;
    }

    if (!rtl8168_write8(io, RTL8168_REG_CFG9346,
                        (uint8_t)RTL8168_CFG9346_LOCK)) {
        return RTL8168_IO_ERROR;
    }

    /* PCI commit: a read of any register flushes the writes above. */
    if (!rtl8168_read8(io, RTL8168_REG_CHIP_CMD, &commit)) {
        return RTL8168_IO_ERROR;
    }

    if (!rtl8168_write8(io, RTL8168_REG_CHIP_CMD,
                        (uint8_t)(RTL8168_CHIP_CMD_TX_ENABLE |
                                  RTL8168_CHIP_CMD_RX_ENABLE))) {
        return RTL8168_IO_ERROR;
    }
    /*
     * A write into a mapped MMIO BAR cannot fail visibly, so the enable is
     * confirmed by reading the command register back: a part that did not
     * latch TE|RE must not be published with ready receive/transmit paths.
     */
    if (!rtl8168_read8(io, RTL8168_REG_CHIP_CMD, &commit)) {
        return RTL8168_IO_ERROR;
    }
    if ((commit & (uint8_t)(RTL8168_CHIP_CMD_TX_ENABLE |
                            RTL8168_CHIP_CMD_RX_ENABLE)) !=
        (uint8_t)(RTL8168_CHIP_CMD_TX_ENABLE |
                  RTL8168_CHIP_CMD_RX_ENABLE)) {
        return RTL8168_ENABLE_FAILED;
    }

    if (rtl8168_rx_config_value(chip, &rx_config) != RTL8168_OK) {
        return RTL8168_BAD_STATE;
    }
    if (!rtl8168_write32(io, RTL8168_REG_RX_CONFIG, rx_config)) {
        return RTL8168_IO_ERROR;
    }
    tx_config = rtl8168_tx_config_value(chip);
    if (!rtl8168_write32(io, RTL8168_REG_TX_CONFIG, tx_config)) {
        return RTL8168_IO_ERROR;
    }

    /*
     * Receive filter: an empty multicast hash (MAR0..MAR7 = 0) plus the
     * accept-my-physical/accept-broadcast bits programmed above.  The
     * reference driver writes both words of the hash the same way
     * (rtl_set_rx_mode()).
     */
    for (index = 0U; index < 2U; ++index) {
        if (!rtl8168_write32(
                io, RTL8168_REG_MAR0 + index * 4U, 0U)) {
            return RTL8168_IO_ERROR;
        }
    }
    return RTL8168_OK;
}

enum rtl8168_status rtl8168_bring_up(
    struct rtl8168_mmio_io *io,
    const struct rtl8168_chip *chip,
    const struct rtl8168_bring_up *configuration,
    const uint8_t mac[RTL8168_MAC_LENGTH])
{
    if (io == NULL || configuration == NULL || mac == NULL) {
        return RTL8168_NULL;
    }
    if (!rtl8168_mac_valid(mac)) {
        return RTL8168_INVALID_MAC;
    }
    if (configuration->rx_buffer_capacity == 0U ||
        configuration->rx_buffer_capacity > RTL8168_RX_BUFFER_SIZE_MAX ||
        (configuration->rx_buffer_capacity % RTL8168_BUFFER_ALIGNMENT) != 0U) {
        return RTL8168_INVALID_LENGTH;
    }
    if (!rtl8168_dma_address_supported(
            configuration->rx_descriptor_physical) ||
        !rtl8168_dma_address_supported(
            configuration->tx_descriptor_physical)) {
        return RTL8168_UNSUPPORTED_DMA_ADDRESS;
    }
    if ((configuration->rx_descriptor_physical %
         RTL8168_DESCRIPTOR_ALIGNMENT) != 0U ||
        (configuration->tx_descriptor_physical %
         RTL8168_DESCRIPTOR_ALIGNMENT) != 0U) {
        return RTL8168_BAD_STATE;
    }

    if (rtl8168_reset(io) != RTL8168_OK) {
        return RTL8168_RESET_TIMEOUT;
    }
    return rtl8168_mac_stage(io, chip, configuration, mac);
}

const char *rtl8168_stage_string(enum rtl8168_cold_boot_stage stage)
{
    switch (stage) {
    case RTL8168_STAGE_RESET:
        return "reset";
    case RTL8168_STAGE_PHY_RESUME:
        return "phy_resume";
    case RTL8168_STAGE_PHY_TUNE:
        return "phy_tune";
    case RTL8168_STAGE_PHY_RESET:
        return "phy_reset";
    case RTL8168_STAGE_EPHY:
        return "ephy";
    case RTL8168_STAGE_CLOCK:
        return "clock";
    case RTL8168_STAGE_MAC:
        return "mac";
    case RTL8168_STAGE_PHY:
        return "phy";
    case RTL8168_STAGE_ANEG:
        return "aneg_start";
    default:
        return "unknown";
    }
}

const char *rtl8168_stage_result_string(enum rtl8168_stage_result result)
{
    switch (result) {
    case RTL8168_STAGE_OK:
        return "OK";
    case RTL8168_STAGE_FAILED:
        return "FAILED";
    case RTL8168_STAGE_SKIPPED:
        return "SKIPPED";
    default:
        return "UNKNOWN";
    }
}

/*
 * ITEM-592: the cold start, in the reference driver's order for this profile.
 *
 * The order is the one the comparison established:
 *
 *   reset  - ChipCmd = CmdReset (the reference driver's rtl_hw_reset);
 *   phy_resume / phy_tune / phy_reset - EVL PHY preparation, including its
 *            own initiated BMCR reset, before the MAC is enabled;
 *   ephy   - the 8168evl EPHY table, before the MAC is enabled;
 *   clock  - the 8168evl clock/FIFO/MCU/EEE programming, before the MAC;
 *   mac    - the ordinary MAC start (config unlock, descriptors, enable);
 *   phy    - page 0, the 10/100 and 1000BASE-T advertisements, BMCR
 *            auto-negotiation, and the read-backs;
 *   aneg_start - whether the BMCR restart command was issued. Completion is
 *            observed separately from BMSR, including on later refreshes.
 *
 * A stage that does not apply to the confirmed profile is SKIPPED by name,
 * never silently omitted: the record then says which sequence the boot took.
 */
static enum rtl8168_status rtl8168_evl_prepare_phy(
    struct rtl8168_mmio_io *io, struct rtl8168_phy_state *phy,
    struct rtl8168_cold_boot_report *report);

enum rtl8168_status rtl8168_cold_boot(
    struct rtl8168_mmio_io *io,
    const struct rtl8168_chip *chip,
    const struct rtl8168_bring_up *configuration,
    const uint8_t mac[RTL8168_MAC_LENGTH],
    struct rtl8168_phy_state *phy,
    struct rtl8168_cold_boot_report *report)
{
    enum rtl8168_status status;
    uint8_t status_byte = 0U;
    size_t index;

    if (io == NULL || chip == NULL || configuration == NULL || mac == NULL ||
        report == NULL) {
        return RTL8168_NULL;
    }
    memset(report, 0, sizeof(*report));
    report->profile = chip->profile;
    report->profile_name = rtl8168_profile_string(chip->profile);
    report->first_failure = RTL8168_OK;
    for (index = 0U; index < RTL8168_STAGE_COUNT; ++index) {
        report->stages[index] = RTL8168_STAGE_SKIPPED;
    }
    /*
     * The reference driver applies a vendor firmware image to the PHY MCU on
     * this generation (`rtl8168e-3.fw`); HFOC carries no vendor blob, so the
     * report names the dependency it did not satisfy rather than pretending
     * the reference sequence was followed in full.
     */
    report->firmware_skipped =
        chip->profile == RTL8168_PROFILE_8168E ||
        chip->profile == RTL8168_PROFILE_8168EVL;
    if (phy != NULL) {
        memset(phy, 0, sizeof(*phy));
        /* ITEM-589/592: the access method the row names is part of the
           answer, whether or not this profile's PHY is driven. */
        phy->access = rtl8168_phy_access_method(chip);
        phy->gigabit = rtl8168_profile_phy_gigabit_mii(chip);
        /* The firmware's own answer before this driver touches anything. */
        if (rtl8168_read8(io, RTL8168_REG_PHY_STATUS, &status_byte)) {
            phy->phy_status_initial = status_byte;
            phy->phy_status_initial_valid = true;
            phy->phy_status_latest = status_byte;
            phy->phy_status_latest_valid = true;
        }
    }

    status = rtl8168_reset(io);
    report->stages[RTL8168_STAGE_RESET] =
        status == RTL8168_OK ? RTL8168_STAGE_OK : RTL8168_STAGE_FAILED;
    if (status != RTL8168_OK) {
        goto fail;
    }
    if (rtl8168_profile_evl_cold_boot(chip)) {
        if (phy != NULL) {
            status = rtl8168_evl_prepare_phy(io, phy, report);
            if (status != RTL8168_OK) {
                goto fail;
            }
        }
        status = rtl8168_evl_ephy_stage(io);
        report->stages[RTL8168_STAGE_EPHY] =
            status == RTL8168_OK ? RTL8168_STAGE_OK : RTL8168_STAGE_FAILED;
        if (status != RTL8168_OK) {
            goto fail;
        }
        status = rtl8168_evl_clock_stage(io);
        report->stages[RTL8168_STAGE_CLOCK] =
            status == RTL8168_OK ? RTL8168_STAGE_OK : RTL8168_STAGE_FAILED;
        if (status != RTL8168_OK) {
            goto fail;
        }
    }

    status = rtl8168_mac_stage(io, chip, configuration, mac);
    report->stages[RTL8168_STAGE_MAC] =
        status == RTL8168_OK ? RTL8168_STAGE_OK : RTL8168_STAGE_FAILED;
    if (status != RTL8168_OK) {
        goto fail;
    }

    if (phy != NULL && rtl8168_phy_mii_supported(chip)) {
        status = rtl8168_phy_configure(io, phy);
        report->stages[RTL8168_STAGE_PHY] =
            status == RTL8168_OK ? RTL8168_STAGE_OK : RTL8168_STAGE_FAILED;
        if (phy->aneg_start_attempted) {
            report->stages[RTL8168_STAGE_ANEG] = phy->aneg_start_issued
                ? RTL8168_STAGE_OK : RTL8168_STAGE_FAILED;
        }
        if (status != RTL8168_OK && report->first_failure == RTL8168_OK) {
            /*
             * A refused PHY is not a dead MAC: the part is bound, the link is
             * down, and the `[NET-PHY]` record names the refusal and the first
             * error.  The MAC stages above are the fatal ones.
             */
            report->first_failure = status;
            report->failed = true;
            report->failed_stage = RTL8168_STAGE_PHY;
        }
        {
            uint8_t status_byte = 0U;

            if (rtl8168_read8(io, RTL8168_REG_PHY_STATUS, &status_byte)) {
                phy->phy_status_latest = status_byte;
                phy->phy_status_latest_valid = true;
            } else {
                phy->phy_status_latest_valid = false;
            }
        }
    }
    return RTL8168_OK;

fail:
    report->failed = true;
    report->failed_stage = RTL8168_STAGE_RESET;
    for (index = 0U; index < RTL8168_STAGE_COUNT; ++index) {
        if (report->stages[index] == RTL8168_STAGE_FAILED) {
            report->failed_stage = (enum rtl8168_cold_boot_stage)index;
            break;
        }
    }
    report->first_failure = status;
    return status;
}

enum rtl8168_status rtl8168_link_status(
    struct rtl8168_mmio_io *io,
    bool *link_up)
{
    uint8_t status;

    if (io == NULL || link_up == NULL) {
        return RTL8168_NULL;
    }
    *link_up = false;
    if (!rtl8168_read8(io, RTL8168_REG_PHY_STATUS, &status)) {
        return RTL8168_IO_ERROR;
    }
    *link_up = (status & RTL8168_PHY_STATUS_LINK) != 0U;
    return RTL8168_OK;
}

/*
 * ITEM-583: clause-22 MII through PHYAR.
 *
 * The owner's RTL8168 rev 07 reported `link=DOWN` while the cable was in a
 * live switch: the driver only ever read the PHYstatus byte and never told
 * the PHY to leave the power-down/isolate state or to start auto-negotiation.
 * The reference r8169 driver uses this same handshake for the part:
 *
 *   write:  PHYAR = 0x80000000 | (reg << 16) | data, then wait busy clear
 *   read:   PHYAR =             (reg << 16),        then wait busy set
 *
 * Every wait is bounded, and both the register selector and the data are
 * masked before they reach the device.
 */
static bool rtl8168_phy_can_wait(const struct rtl8168_mmio_io *io);

/*
 * ITEM-592: one bounded handshake for both PHY doorbells.
 *
 * PHYAR (0x60) and EPHYAR (0x80) share the shape - bit 31 is the completion
 * flag, bits 20..16 the sub-register, bits 15..0 the data - and differ only in
 * their timing, so the loop is written once and parameterised.  Each loop is
 * the reference driver's own: one check, then a sleep, a bounded number of
 * times (PHYAR 25 us x 20, EPHYAR 10 us x 100), so a part that never answers
 * is given up inside a measured budget instead of a tight loop.
 */
static enum rtl8168_status rtl8168_window_wait(
    struct rtl8168_mmio_io *io,
    uint16_t reg,
    uint32_t flag,
    bool high,
    uint32_t poll_us,
    uint32_t polls,
    uint32_t *last,
    struct rtl8168_phy_transaction *transaction)
{
    uint32_t value = 0U;
    uint32_t attempt;

    if (!rtl8168_phy_can_wait(io)) {
        return RTL8168_IO_ERROR;
    }
    for (attempt = 0U; attempt < polls; ++attempt) {
        if (transaction != NULL) {
            transaction->polls = attempt + 1U;
        }
        if (!rtl8168_read32(io, reg, &value)) {
            if (transaction != NULL) {
                transaction->reason = RTL8168_PHY_FAILURE_MMIO_READ;
            }
            return RTL8168_IO_ERROR;
        }
        if (transaction != NULL) {
            transaction->last = value;
            transaction->last_valid = true;
        }
        if (((value & flag) != 0U) == high) {
            if (last != NULL) {
                *last = value;
            }
            return RTL8168_OK;
        }
        io->delay_us(io->context, poll_us);
    }
    if (transaction != NULL) {
        transaction->reason = RTL8168_PHY_FAILURE_TIMEOUT;
    }
    return RTL8168_IO_ERROR;
}

static enum rtl8168_status rtl8168_phy_wait(
    struct rtl8168_mmio_io *io,
    bool busy,
    uint32_t *last)
{
    return rtl8168_window_wait(io, RTL8168_REG_PHY_ACCESS,
                               RTL8168_PHY_ACCESS_BUSY, busy,
                               RTL8168_PHY_ACCESS_POLL_US,
                               RTL8168_PHY_ACCESS_POLLS, last, &io->phy_last);
}

const char *rtl8168_phy_failure_reason_string(enum rtl8168_phy_failure_reason r)
{
    switch (r) {
    case RTL8168_PHY_FAILURE_NONE: return "NONE";
    case RTL8168_PHY_FAILURE_NO_DELAY: return "NO_DELAY";
    case RTL8168_PHY_FAILURE_MMIO_WRITE: return "MMIO_WRITE_FAILED";
    case RTL8168_PHY_FAILURE_MMIO_READ: return "MMIO_READ_FAILED";
    case RTL8168_PHY_FAILURE_TIMEOUT: return "PHYAR_TIMEOUT";
    case RTL8168_PHY_FAILURE_PAGE_MISMATCH: return "PAGE_MISMATCH";
    case RTL8168_PHY_FAILURE_RESET_TIMEOUT: return "RESET_TIMEOUT";
    case RTL8168_PHY_FAILURE_BAD_REGISTER: return "BAD_REGISTER";
    default: return "UNKNOWN";
    }
}

const char *rtl8168_phy_phase_string(enum rtl8168_phy_phase phase)
{
    switch (phase) {
    case RTL8168_PHY_PHASE_DIRECT: return "DIRECT";
    case RTL8168_PHY_PHASE_PAGE: return "PAGE";
    case RTL8168_PHY_PHASE_SELECTOR: return "SELECTOR";
    case RTL8168_PHY_PHASE_READ: return "READ";
    case RTL8168_PHY_PHASE_WRITE: return "WRITE";
    case RTL8168_PHY_PHASE_RESTORE: return "RESTORE";
    case RTL8168_PHY_PHASE_RESET_WAIT: return "RESET_WAIT";
    default: return "UNKNOWN";
    }
}

static void rtl8168_phy_transaction_begin(struct rtl8168_mmio_io *io,
                                         uint8_t reg, bool write, uint16_t data)
{
    memset(&io->phy_last, 0, sizeof(io->phy_last));
    io->phy_page_write_valid = false;
    io->phy_last.reg = reg;
    io->phy_last.write = write;
    io->phy_last.command = ((uint32_t)reg << RTL8168_PHY_ACCESS_REG_SHIFT) |
                          (write ? RTL8168_PHY_ACCESS_WRITE | data : 0U);
    io->phy_last.expected = data;
    io->phy_last.expected_valid = write;
}

/*
 * ITEM-591: the pause after a completion indication.
 *
 * The hardware spec requires a gap before the next PHYAR command, and the
 * reference driver keeps it on the failure path as well (`udelay(20)` after
 * the read regardless of the timeout).  A bus with no microsecond pause cannot
 * honour it, so the PHY access refuses such a bus before issuing anything
 * rather than issuing commands the part was not ready for.
 */
static bool rtl8168_phy_can_wait(const struct rtl8168_mmio_io *io)
{
    return io->delay_us != NULL;
}

static void rtl8168_phy_settle(struct rtl8168_mmio_io *io)
{
    if (io->delay_us != NULL) {
        io->delay_us(io->context, RTL8168_PHY_ACCESS_SETTLE_US);
    }
}

enum rtl8168_phy_access rtl8168_phy_access_method(
    const struct rtl8168_chip *chip)
{
    size_t index;

    if (chip == NULL) {
        return RTL8168_PHY_ACCESS_NONE;
    }
    for (index = 0U;
         index < sizeof(rtl8168_xid_table) / sizeof(rtl8168_xid_table[0]);
         ++index) {
        if ((chip->xid & rtl8168_xid_table[index].mask) ==
            rtl8168_xid_table[index].value) {
            return rtl8168_xid_table[index].access;
        }
    }
    return RTL8168_PHY_ACCESS_NONE;
}

const char *rtl8168_phy_access_string(enum rtl8168_phy_access access)
{
    switch (access) {
    case RTL8168_PHY_ACCESS_MII_PHYAR:
        return "MII_PHYAR";
    case RTL8168_PHY_ACCESS_OCP:
        return "OCP";
    default:
        return "NONE";
    }
}

bool rtl8168_phy_mii_supported(const struct rtl8168_chip *chip)
{
    return rtl8168_phy_access_method(chip) == RTL8168_PHY_ACCESS_MII_PHYAR;
}

enum rtl8168_status rtl8168_phy_read(
    struct rtl8168_mmio_io *io,
    uint8_t register_index,
    uint16_t *value)
{
    uint32_t payload = 0U;

    if (io == NULL || value == NULL) {
        return RTL8168_NULL;
    }
    rtl8168_phy_transaction_begin(io, register_index, false, 0U);
    *value = 0U;
    if ((register_index & ~RTL8168_PHY_ACCESS_REG_MASK) != 0U) {
        io->phy_last.reason = RTL8168_PHY_FAILURE_BAD_REGISTER;
        return RTL8168_BAD_STATE;
    }
    if (!rtl8168_phy_can_wait(io)) {
        io->phy_last.reason = RTL8168_PHY_FAILURE_NO_DELAY;
        return RTL8168_IO_ERROR;
    }
    if (!rtl8168_write32(
            io, RTL8168_REG_PHY_ACCESS,
            (uint32_t)register_index << RTL8168_PHY_ACCESS_REG_SHIFT)) {
        io->phy_last.reason = RTL8168_PHY_FAILURE_MMIO_WRITE;
        return RTL8168_IO_ERROR;
    }
    {
        const enum rtl8168_status status =
            rtl8168_phy_wait(io, true, NULL);

        if (status != RTL8168_OK) {
            rtl8168_phy_settle(io);
            return RTL8168_IO_ERROR;
        }
        /*
         * ITEM-595: completion and payload are separate reads in Linux
         * r8169_mdio_read.  The completion sample does not guarantee stable
         * low16 data; retain it in phy_last.last and read the actual payload
         * now, before the required post-operation settle.
         */
        if (!rtl8168_read32(io, RTL8168_REG_PHY_ACCESS, &payload)) {
            io->phy_last.reason = RTL8168_PHY_FAILURE_MMIO_READ;
            rtl8168_phy_settle(io);
            return RTL8168_IO_ERROR;
        }
        rtl8168_phy_settle(io);
    }
    *value = (uint16_t)(payload & RTL8168_PHY_ACCESS_DATA_MASK);
    io->phy_last.observed = *value;
    io->phy_last.observed_valid = true;
    io->phy_last.payload = payload;
    io->phy_last.payload_valid = true;
    return RTL8168_OK;
}

enum rtl8168_status rtl8168_phy_write(
    struct rtl8168_mmio_io *io,
    uint8_t register_index,
    uint16_t value)
{
    if (io == NULL) {
        return RTL8168_NULL;
    }
    rtl8168_phy_transaction_begin(io, register_index, true, value);
    if ((register_index & ~RTL8168_PHY_ACCESS_REG_MASK) != 0U) {
        io->phy_last.reason = RTL8168_PHY_FAILURE_BAD_REGISTER;
        return RTL8168_BAD_STATE;
    }
    if (!rtl8168_phy_can_wait(io)) {
        io->phy_last.reason = RTL8168_PHY_FAILURE_NO_DELAY;
        return RTL8168_IO_ERROR;
    }
    if (!rtl8168_write32(
            io, RTL8168_REG_PHY_ACCESS,
            RTL8168_PHY_ACCESS_WRITE |
                ((uint32_t)register_index << RTL8168_PHY_ACCESS_REG_SHIFT) |
                (uint32_t)value)) {
        io->phy_last.reason = RTL8168_PHY_FAILURE_MMIO_WRITE;
        return RTL8168_IO_ERROR;
    }
    {
        const enum rtl8168_status status = rtl8168_phy_wait(io, false, NULL);

        rtl8168_phy_settle(io);
        if (status != RTL8168_OK) {
            return RTL8168_IO_ERROR;
        }
    }
    return RTL8168_OK;
}

enum rtl8168_status rtl8168_ephy_read(
    struct rtl8168_mmio_io *io,
    uint8_t register_index,
    uint16_t *value)
{
    uint32_t result = 0U;
    enum rtl8168_status status;

    if (io == NULL || value == NULL) {
        return RTL8168_NULL;
    }
    *value = 0U;
    if ((register_index & ~RTL8168_EPHY_ACCESS_REG_MASK) != 0U) {
        return RTL8168_BAD_STATE;
    }
    if (!rtl8168_phy_can_wait(io)) {
        return RTL8168_IO_ERROR;
    }
    if (!rtl8168_write32(
            io, RTL8168_REG_EPHY_ACCESS,
            (uint32_t)register_index << RTL8168_EPHY_ACCESS_REG_SHIFT)) {
        return RTL8168_IO_ERROR;
    }
    status = rtl8168_window_wait(io, RTL8168_REG_EPHY_ACCESS,
                                 RTL8168_EPHY_ACCESS_FLAG, true,
                                 RTL8168_EPHY_ACCESS_POLL_US,
                                 RTL8168_EPHY_ACCESS_POLLS, &result, NULL);
    io->delay_us(io->context, RTL8168_EPHY_ACCESS_SETTLE_US);
    if (status != RTL8168_OK) {
        return RTL8168_IO_ERROR;
    }
    *value = (uint16_t)(result & RTL8168_EPHY_ACCESS_DATA_MASK);
    return RTL8168_OK;
}

enum rtl8168_status rtl8168_ephy_write(
    struct rtl8168_mmio_io *io,
    uint8_t register_index,
    uint16_t value)
{
    enum rtl8168_status status;

    if (io == NULL) {
        return RTL8168_NULL;
    }
    if ((register_index & ~RTL8168_EPHY_ACCESS_REG_MASK) != 0U) {
        return RTL8168_BAD_STATE;
    }
    if (!rtl8168_phy_can_wait(io)) {
        return RTL8168_IO_ERROR;
    }
    if (!rtl8168_write32(
            io, RTL8168_REG_EPHY_ACCESS,
            RTL8168_EPHY_ACCESS_WRITE |
                ((uint32_t)register_index << RTL8168_EPHY_ACCESS_REG_SHIFT) |
                (uint32_t)value)) {
        return RTL8168_IO_ERROR;
    }
    status = rtl8168_window_wait(io, RTL8168_REG_EPHY_ACCESS,
                                 RTL8168_EPHY_ACCESS_FLAG, false,
                                 RTL8168_EPHY_ACCESS_POLL_US,
                                 RTL8168_EPHY_ACCESS_POLLS, NULL, NULL);
    io->delay_us(io->context, RTL8168_EPHY_ACCESS_SETTLE_US);
    if (status != RTL8168_OK) {
        return RTL8168_IO_ERROR;
    }
    return RTL8168_OK;
}

/*
 * ITEM-592: select the PHY's page.
 *
 * Registers 0..15 are the standard clause-22 set only while the page register
 * (0x1f) is zero.  Firmware can leave the part on another page, and a read of
 * "BMCR" on page 7 is not BMCR at all - the cold start therefore selects page
 * 0 before it interprets anything, and the state records the page it settled
 * on.  The read-back is the page the part reports, so a PHY that ignores the
 * selection is visible instead of silently misread.
 */
enum rtl8168_status rtl8168_phy_select_page(
    struct rtl8168_mmio_io *io,
    uint16_t page)
{
    uint16_t observed = 0U;
    struct rtl8168_phy_transaction page_write;

    if (io == NULL) {
        return RTL8168_NULL;
    }
    if (rtl8168_phy_write(io, RTL8168_PHY_REG_PAGE, page) != RTL8168_OK) {
        return RTL8168_IO_ERROR;
    }
    page_write = io->phy_last;
    if (rtl8168_phy_read(io, RTL8168_PHY_REG_PAGE, &observed) !=
        RTL8168_OK) {
        io->phy_page_write = page_write;
        io->phy_page_write_valid = true;
        return RTL8168_IO_ERROR;
    }
    if (observed != page) {
        io->phy_last.reason = RTL8168_PHY_FAILURE_PAGE_MISMATCH;
        io->phy_last.expected = page;
        io->phy_last.expected_valid = true;
        io->phy_page_write = page_write;
        io->phy_page_write_valid = true;
        return RTL8168_IO_ERROR;
    }
    return RTL8168_OK;
}

/* ITEM-597: only the VER34 vendor recipe uses this policy. Linux v6.12's
 * phy_select_page writes a selector without requiring its echo. On Toshiba,
 * an atomic Linux probe read1f=0 on page3 while reg9=a20f, then reg9=0300
 * on page0. Retain zero as UNVERIFIED, never synthesize bank acceptance.
 * Page0/public selection stay strict; other mismatches and IO still refuse.
 * The PHYAR operations and their settle delays are unchanged. */
static enum rtl8168_status rtl8168_evl_select_vendor_page(
    struct rtl8168_mmio_io *io, struct rtl8168_phy_state *phy, uint16_t page)
{
    struct rtl8168_phy_transaction write;
    uint16_t observed = 0U;

    if (page == RTL8168_PHY_PAGE_STANDARD) {
        return rtl8168_phy_select_page(io, page);
    }
    if (rtl8168_phy_write(io, RTL8168_PHY_REG_PAGE, page) != RTL8168_OK) {
        return RTL8168_IO_ERROR;
    }
    write = io->phy_last;
    if (rtl8168_phy_read(io, RTL8168_PHY_REG_PAGE, &observed) != RTL8168_OK) {
        io->phy_page_write = write;
        io->phy_page_write_valid = true;
        return RTL8168_IO_ERROR;
    }
    if (observed == 0U) {
        if (phy->page_selection.zero_reads == 0U) {
            phy->page_selection.requested = page;
            phy->page_selection.write = write;
            phy->page_selection.read = io->phy_last;
        }
        ++phy->page_selection.zero_reads;
    } else if (observed != page) {
        io->phy_last.reason = RTL8168_PHY_FAILURE_PAGE_MISMATCH;
        io->phy_last.expected = page;
        io->phy_last.expected_valid = true;
        io->phy_page_write = write;
        io->phy_page_write_valid = true;
        return RTL8168_IO_ERROR;
    }
    return RTL8168_OK;
}

static enum rtl8168_status rtl8168_phy_clear_reset(
    struct rtl8168_mmio_io *io,
    uint16_t *bmcr)
{
    uint32_t attempt;

    for (attempt = 0U; attempt < RTL8168_PHY_RESET_POLLS; ++attempt) {
        uint16_t value = 0U;

        if (rtl8168_phy_read(io, RTL8168_PHY_REG_BMCR, &value) !=
            RTL8168_OK) {
            return RTL8168_IO_ERROR;
        }
        if ((value & RTL8168_PHY_BMCR_RESET) == 0U) {
            *bmcr = value;
            return RTL8168_OK;
        }
    }
    io->phy_last.reason = RTL8168_PHY_FAILURE_RESET_TIMEOUT;
    return RTL8168_IO_ERROR;
}

/* ITEM-589: the first PHY error a state ever sees is the one worth keeping. */
static void rtl8168_phy_note_first_error(struct rtl8168_phy_state *state)
{
    if (state->first_error == RTL8168_OK) {
        state->first_error = RTL8168_IO_ERROR;
    }
}

/* A failed step of the configuration itself: the verdict is REFUSED. */
static void rtl8168_phy_note_error(struct rtl8168_phy_state *state)
{
    state->config_status = RTL8168_IO_ERROR;
    rtl8168_phy_note_first_error(state);
}

static void rtl8168_phy_keep_failure(
    const struct rtl8168_mmio_io *io, struct rtl8168_phy_state *state,
    uint8_t step, enum rtl8168_phy_phase phase, uint16_t page, uint16_t selector)
{
    if (state->failure.valid || io->phy_last.reason == RTL8168_PHY_FAILURE_NONE) {
        return;
    }
    state->failure.valid = true;
    state->failure.step = step;
    state->failure.phase = phase;
    state->failure.page = page;
    state->failure.selector = selector;
    state->failure.selector_valid = selector != 0xffffU;
    state->failure.transaction = io->phy_last;
    state->failure.page_write_valid = io->phy_page_write_valid;
    if (io->phy_page_write_valid) {
        state->failure.page_write = io->phy_page_write;
    }
}

static void rtl8168_phy_note_access_first_error(
    const struct rtl8168_mmio_io *io, struct rtl8168_phy_state *state)
{
    rtl8168_phy_keep_failure(io, state, 0U, RTL8168_PHY_PHASE_DIRECT, 0U, 0xffffU);
    rtl8168_phy_note_first_error(state);
}

static void rtl8168_phy_note_access_error(
    const struct rtl8168_mmio_io *io, struct rtl8168_phy_state *state)
{
    rtl8168_phy_keep_failure(io, state, 0U, RTL8168_PHY_PHASE_DIRECT, 0U, 0xffffU);
    rtl8168_phy_note_error(state);
}

/* ITEM-593: independently expressed hardware operations for VER_34 only.
 * References, applicability, source digests and firmware limitations:
 * docs/rtl8168evl-phy-audit.md. No firmware instructions are substituted here.
 * A selector of 0xffff means an ordinary paged register. */
struct rtl8168_evl_phy_setting {
    uint16_t page;
    uint16_t selector;
    uint8_t reg;
    uint16_t clear;
    uint16_t set;
};

static const struct rtl8168_evl_phy_setting rtl8168_evl_phy_settings[] = {
    {7U, 0x00acU, 0x18U, 0xffffU, 0x0006U}, /* delay cap */
    {3U, 0xffffU, 0x09U, 0xffffU, 0xa20fU}, /* channel estimation */
    {5U, 0x8b5bU, 0x06U, 0xffffU, 0x9222U},
    {5U, 0x8b6dU, 0x06U, 0xffffU, 0x8000U},
    {5U, 0x8b76U, 0x06U, 0xffffU, 0x8000U},
    {5U, 0x8b80U, 0x17U, 0x0000U, 0x0006U}, /* corner: register 17 */
    {7U, 0x002dU, 0x18U, 0x0000U, 0x0010U},
    {0U, 0xffffU, 0x14U, 0x0000U, 0x8000U},
    {5U, 0x8b86U, 0x06U, 0x0000U, 0x0001U}, /* 10M waveform */
    {5U, 0x8b85U, 0x06U, 0x0000U, 0x4000U}, /* two-pair detection */
    {7U, 0x0020U, 0x15U, 0x0000U, 0x0100U}, /* EEE PHY */
    {5U, 0x8b85U, 0x06U, 0x0000U, 0x2000U},
    {3U, 0xffffU, 0x19U, 0x0000U, 0x0001U},
    {3U, 0xffffU, 0x10U, 0x0000U, 0x0400U},
    {5U, 0xffffU, 0x01U, 0x0000U, 0x0100U},
};

static enum rtl8168_status rtl8168_evl_wait_phy_reset(
    struct rtl8168_mmio_io *io, uint16_t *bmcr)
{
    if (io->delay_us == NULL) {
        return RTL8168_IO_ERROR;
    }
    for (uint32_t poll = 0U; poll < RTL8168_PHY_RESET_WAIT_POLLS; ++poll) {
        io->delay_us(io->context, RTL8168_PHY_RESET_POLL_US);
        if (rtl8168_phy_read(io, RTL8168_PHY_REG_BMCR, bmcr) != RTL8168_OK) {
            return RTL8168_IO_ERROR;
        }
        if ((*bmcr & RTL8168_PHY_BMCR_RESET) == 0U) {
            io->delay_us(io->context, RTL8168_PHY_RESET_SETTLE_US);
            return RTL8168_OK;
        }
    }
    io->phy_last.reason = RTL8168_PHY_FAILURE_RESET_TIMEOUT;
    return RTL8168_IO_ERROR;
}

static enum rtl8168_status rtl8168_evl_prepare_phy(
    struct rtl8168_mmio_io *io, struct rtl8168_phy_state *phy,
    struct rtl8168_cold_boot_report *report)
{
    enum rtl8168_cold_boot_stage stage = RTL8168_STAGE_PHY_RESUME;
    enum rtl8168_status status = RTL8168_IO_ERROR;
    uint16_t bmcr = 0U;
    uint8_t step = 0U;
    enum rtl8168_phy_phase phase = RTL8168_PHY_PHASE_PAGE;
    uint16_t page = 0U;
    uint16_t selector = 0xffffU;

    phy->configure_attempted = true;
    phy->config_status = RTL8168_IO_ERROR;
    if (rtl8168_phy_select_page(io, RTL8168_PHY_PAGE_STANDARD) != RTL8168_OK) {
        goto done;
    }
    phase = RTL8168_PHY_PHASE_READ;
    if (rtl8168_phy_read(io, RTL8168_PHY_REG_BMCR, &bmcr) != RTL8168_OK) {
        goto done;
    }
    phy->bmcr_initial = bmcr;
    phy->bmcr_initial_valid = true;
    phase = RTL8168_PHY_PHASE_RESET_WAIT;
    if ((bmcr & RTL8168_PHY_BMCR_RESET) != 0U &&
        rtl8168_evl_wait_phy_reset(io, &bmcr) != RTL8168_OK) {
        goto done;
    }
    bmcr &= (uint16_t)~RTL8168_PHY_BMCR_POWER_DOWN;
    phase = RTL8168_PHY_PHASE_WRITE;
    if (rtl8168_phy_write(io, RTL8168_PHY_REG_BMCR, bmcr) != RTL8168_OK) {
        goto done;
    }
    report->stages[stage] = RTL8168_STAGE_OK;
    stage = RTL8168_STAGE_PHY_TUNE;
    for (size_t i = 0U; i < sizeof(rtl8168_evl_phy_settings) /
                             sizeof(rtl8168_evl_phy_settings[0]); ++i) {
        const struct rtl8168_evl_phy_setting *setting =
            &rtl8168_evl_phy_settings[i];
        uint16_t value = 0U;
        step = (uint8_t)(i + 1U);
        page = setting->page;
        selector = setting->selector;
        phase = RTL8168_PHY_PHASE_PAGE;
        if (rtl8168_evl_select_vendor_page(io, phy, setting->page) != RTL8168_OK) {
            goto done;
        }
        phase = RTL8168_PHY_PHASE_SELECTOR;
        if (setting->selector != 0xffffU &&
            rtl8168_phy_write(io, setting->page == 7U ? 0x1eU : 0x05U,
                              setting->selector) != RTL8168_OK) {
            goto done;
        }
        phase = RTL8168_PHY_PHASE_READ;
        if (rtl8168_phy_read(io, setting->reg, &value) != RTL8168_OK) {
            goto done;
        }
        phase = RTL8168_PHY_PHASE_WRITE;
        if (rtl8168_phy_write(io, setting->reg,
                              (uint16_t)((value & (uint16_t)~setting->clear) |
                                         setting->set)) != RTL8168_OK) {
            goto done;
        }
        phase = RTL8168_PHY_PHASE_RESTORE;
        page = 0U;
        selector = 0xffffU;
        if (rtl8168_phy_select_page(io, RTL8168_PHY_PAGE_STANDARD) != RTL8168_OK) {
            goto done;
        }
    }
    report->stages[stage] = RTL8168_STAGE_OK;
    stage = RTL8168_STAGE_PHY_RESET;
    step = 0U;
    page = 0U;
    selector = 0xffffU;
    phase = RTL8168_PHY_PHASE_READ;
    if (rtl8168_phy_read(io, RTL8168_PHY_REG_BMCR, &bmcr) != RTL8168_OK) {
        goto done;
    }
    phase = RTL8168_PHY_PHASE_WRITE;
    if (rtl8168_phy_write(io, RTL8168_PHY_REG_BMCR,
                          (uint16_t)((bmcr &
                                      (uint16_t)~RTL8168_PHY_BMCR_ISOLATE) |
                                     RTL8168_PHY_BMCR_RESET |
                                     RTL8168_PHY_BMCR_RESTART_ANEG)) !=
            RTL8168_OK) {
        goto done;
    }
    phase = RTL8168_PHY_PHASE_RESET_WAIT;
    if (rtl8168_evl_wait_phy_reset(io, &bmcr) != RTL8168_OK) {
        goto done;
    }
    report->stages[stage] = RTL8168_STAGE_OK;
    status = RTL8168_OK;

done:
    if (status != RTL8168_OK) {
        rtl8168_phy_keep_failure(io, phy, step, phase, page, selector);
    }
    /* Always attempt recovery, even if selecting the vendor page failed.
     * The operation's first failed stage survives a second cleanup failure. */
    phy->page_valid = false;
    if (rtl8168_phy_select_page(io, RTL8168_PHY_PAGE_STANDARD) == RTL8168_OK) {
        phy->page = RTL8168_PHY_PAGE_STANDARD;
        phy->page_valid = true;
    } else {
        rtl8168_phy_keep_failure(io, phy, step, RTL8168_PHY_PHASE_RESTORE,
                                 0U, 0xffffU);
        status = RTL8168_IO_ERROR;
    }
    if (phy->failure.valid && !phy->failure.restore_attempted) {
        phy->failure.restore_attempted = true;
        phy->failure.restore_ok = phy->page_valid;
        /* A completed nonzero read is evidence even when page0 recovery
         * failed. It belongs to cleanup, never to the frozen first error. */
        phy->failure.restore_page_valid =
            io->phy_last.reg == RTL8168_PHY_REG_PAGE &&
            !io->phy_last.write && io->phy_last.observed_valid;
        phy->failure.restore_page = io->phy_last.observed;
    }
    if (status != RTL8168_OK) {
        report->stages[stage] = RTL8168_STAGE_FAILED;
        rtl8168_phy_note_error(phy);
    }
    return status;
}

/*
 * ITEM-592: the successive BMSR readings.
 *
 * One sample cannot tell "auto-negotiation never started" from "it completed
 * and the link dropped".  The state keeps how many readings were taken, the
 * first few distinct values, and whether the link/AN-complete bits were ever
 * seen at all, which is what a link that stays down needs to be diagnosed.
 */
static void rtl8168_phy_note_bmsr(struct rtl8168_phy_state *state,
                                  uint16_t bmsr)
{
    bool duplicate = false;
    uint8_t index;

    ++state->bmsr_reads;
    state->bmsr = bmsr;
    if ((bmsr & RTL8168_PHY_BMSR_LINK) != 0U) {
        state->link_seen = true;
    }
    if ((bmsr & RTL8168_PHY_BMSR_ANEG_COMPLETE) != 0U) {
        state->aneg_complete_seen = true;
    }
    for (index = 0U; index < state->bmsr_sequence_count; ++index) {
        if (state->bmsr_sequence[index] == bmsr) {
            duplicate = true;
            break;
        }
    }
    if (!duplicate && state->bmsr_sequence_count < RTL8168_PHY_BMSR_SAMPLES) {
        state->bmsr_sequence[state->bmsr_sequence_count] = bmsr;
        ++state->bmsr_sequence_count;
    }
}

static void rtl8168_phy_note_bmcr(struct rtl8168_phy_state *state,
                                  uint16_t bmcr)
{
    state->bmcr = bmcr;
    state->link_up = (state->bmsr & RTL8168_PHY_BMSR_LINK) != 0U;
    state->autoneg_complete =
        (state->bmsr & RTL8168_PHY_BMSR_ANEG_COMPLETE) != 0U;
    state->powered_down = (bmcr & RTL8168_PHY_BMCR_POWER_DOWN) != 0U;
}

enum rtl8168_status rtl8168_phy_configure(
    struct rtl8168_mmio_io *io,
    struct rtl8168_phy_state *state)
{
    uint16_t bmcr = 0U;
    uint16_t bmsr = 0U;
    uint16_t id1 = 0U;
    uint16_t id2 = 0U;
    uint16_t lpa = 0U;
    uint16_t advertise = 0U;
    uint16_t advertise_1000 = 0U;
    uint16_t lpa_1000 = 0U;
    uint16_t control;

    if (io == NULL || state == NULL) {
        return RTL8168_NULL;
    }
    state->configure_attempted = true;
    state->config_status = RTL8168_IO_ERROR;
    state->aneg_start_issued = false;
    state->aneg_start_attempted = false;
    state->advertise_valid = false;
    state->advertise_1000_valid = false;
    state->lpa_1000_valid = false;
    state->status_valid = false;
    /*
     * ITEM-592: registers 0..15 are the standard clause-22 set only on page 0.
     * A firmware that left the part on another page makes "BMCR" mean
     * something else, so the page is selected and verified before the first
     * interpreted read.  This is also the reference driver's habit: its PHY
     * table ends by writing page 0.
     */
    if (rtl8168_phy_select_page(io, RTL8168_PHY_PAGE_STANDARD) != RTL8168_OK) {
        rtl8168_phy_note_access_error(io, state);
        return RTL8168_IO_ERROR;
    }
    state->page = RTL8168_PHY_PAGE_STANDARD;
    state->page_valid = true;
    if (rtl8168_phy_clear_reset(io, &bmcr) != RTL8168_OK) {
        rtl8168_phy_note_access_error(io, state);
        return RTL8168_IO_ERROR;
    }
    if (!state->bmcr_initial_valid) {
        state->bmcr_initial = bmcr;
        state->bmcr_initial_valid = true;
    }
    /*
     * Leave a firmware power-down or isolate state, disable loopback, keep
     * the part's duplex/speed hints only while auto-negotiation is off, and
     * restart negotiation.
     */
    control = (uint16_t)(bmcr &
                         (uint16_t)~(RTL8168_PHY_BMCR_POWER_DOWN |
                                     RTL8168_PHY_BMCR_ISOLATE |
                                     RTL8168_PHY_BMCR_LOOPBACK |
                                     RTL8168_PHY_BMCR_RESTART_ANEG));
    control |= RTL8168_PHY_BMCR_ANEG_ENABLE | RTL8168_PHY_BMCR_RESTART_ANEG;
    /*
     * ITEM-592: the advertisement is split the way the standard defines it -
     * 10/100 in register 4, 1000BASE-T in register 9 - and written before
     * auto-negotiation is restarted, as the reference driver's advertisement
     * helper does.
     */
    if (rtl8168_phy_write(io, RTL8168_PHY_REG_ADVERTISE,
                          RTL8168_PHY_ADVERTISE_DEFAULT) != RTL8168_OK) {
        rtl8168_phy_note_access_error(io, state);
        return RTL8168_IO_ERROR;
    }
    state->advertise_written = RTL8168_PHY_ADVERTISE_DEFAULT;
    if (state->gigabit) {
        if (rtl8168_phy_write(io, RTL8168_PHY_REG_CTRL1000,
                              RTL8168_PHY_ADVERTISE_1000) != RTL8168_OK) {
            rtl8168_phy_note_access_error(io, state);
            return RTL8168_IO_ERROR;
        }
        state->advertise_1000_written = RTL8168_PHY_ADVERTISE_1000;
    }
    state->aneg_start_attempted = true;
    if (rtl8168_phy_write(io, RTL8168_PHY_REG_BMCR, control) != RTL8168_OK) {
        rtl8168_phy_note_access_error(io, state);
        return RTL8168_IO_ERROR;
    }
    state->aneg_start_issued = true;
    if (rtl8168_phy_read(io, RTL8168_PHY_REG_BMCR, &bmcr) != RTL8168_OK ||
        rtl8168_phy_read(io, RTL8168_PHY_REG_BMSR, &bmsr) != RTL8168_OK) {
        rtl8168_phy_note_access_error(io, state);
        return RTL8168_IO_ERROR;
    }
    /* A completed BMSR read is historical evidence even if a later register
     * fails. The whole live snapshot remains unverified until all reads pass. */
    state->bmcr = bmcr;
    rtl8168_phy_note_bmsr(state, bmsr);
    if (rtl8168_phy_read(io, RTL8168_PHY_REG_PHYID1, &id1) != RTL8168_OK ||
        rtl8168_phy_read(io, RTL8168_PHY_REG_PHYID2, &id2) != RTL8168_OK ||
        rtl8168_phy_read(io, RTL8168_PHY_REG_LPA, &lpa) != RTL8168_OK) {
        rtl8168_phy_note_access_error(io, state);
        return RTL8168_IO_ERROR;
    }
    state->valid = true;
    state->status_valid = true;
    state->phy_id1 = id1;
    state->phy_id2 = id2;
    rtl8168_phy_note_bmcr(state, bmcr);
    state->lpa = lpa;
    /*
     * ITEM-591/592: read the advertisements back instead of reporting the
     * constants.  A PHY that masks bits answers differently, and that
     * difference is exactly what a link that never resolves needs to be read
     * as.  A read that does not answer is its own fact: the first error is
     * kept and the reading marked unverified, while CONFIGURED, the link state
     * and the auto-negotiation result stay separate answers.
     */
    state->advertise = 0U;
    state->advertise_valid = false;
    if (rtl8168_phy_read(io, RTL8168_PHY_REG_ADVERTISE, &advertise) ==
        RTL8168_OK) {
        state->advertise = advertise;
        state->advertise_valid = true;
    } else {
        rtl8168_phy_note_access_first_error(io, state);
    }
    if (state->gigabit) {
        state->advertise_1000 = 0U;
        state->advertise_1000_valid = false;
        if (rtl8168_phy_read(io, RTL8168_PHY_REG_CTRL1000,
                             &advertise_1000) == RTL8168_OK) {
            state->advertise_1000 = advertise_1000;
            state->advertise_1000_valid = true;
        } else {
            rtl8168_phy_note_access_first_error(io, state);
        }
        state->lpa_1000 = 0U;
        state->lpa_1000_valid = false;
        if (rtl8168_phy_read(io, RTL8168_PHY_REG_STAT1000, &lpa_1000) ==
            RTL8168_OK) {
            state->lpa_1000 = lpa_1000;
            state->lpa_1000_valid = true;
        } else {
            rtl8168_phy_note_access_first_error(io, state);
        }
    }
    ++state->configures;
    state->config_status = RTL8168_OK;
    return RTL8168_OK;
}

enum rtl8168_status rtl8168_phy_refresh(
    struct rtl8168_mmio_io *io,
    struct rtl8168_phy_state *state)
{
    uint16_t bmcr = 0U;
    uint16_t bmsr = 0U;
    uint16_t lpa = 0U;
    uint16_t advertise = 0U;
    uint16_t advertise_1000 = 0U;
    uint16_t lpa_1000 = 0U;
    uint8_t status_byte = 0U;

    if (io == NULL || state == NULL) {
        return RTL8168_NULL;
    }
    state->status_valid = false;
    state->advertise_valid = false;
    state->advertise_1000_valid = false;
    state->lpa_1000_valid = false;
    state->phy_status_latest_valid = false;
    state->page_valid = false;
    if (rtl8168_phy_select_page(io, RTL8168_PHY_PAGE_STANDARD) != RTL8168_OK) {
        rtl8168_phy_note_access_first_error(io, state);
        return RTL8168_IO_ERROR;
    }
    state->page = RTL8168_PHY_PAGE_STANDARD;
    state->page_valid = true;
    if (rtl8168_phy_read(io, RTL8168_PHY_REG_BMCR, &bmcr) != RTL8168_OK ||
        rtl8168_phy_read(io, RTL8168_PHY_REG_BMSR, &bmsr) != RTL8168_OK) {
        rtl8168_phy_note_access_first_error(io, state);
        return RTL8168_IO_ERROR;
    }
    state->bmcr = bmcr;
    rtl8168_phy_note_bmsr(state, bmsr);
    if (rtl8168_phy_read(io, RTL8168_PHY_REG_LPA, &lpa) != RTL8168_OK) {
        rtl8168_phy_note_access_first_error(io, state);
        return RTL8168_IO_ERROR;
    }
    /*
     * A PHY that reset itself (a cable event or a firmware action can do it)
     * comes back powered down.  The refresh reports that state instead of
     * hiding it; the caller restarts negotiation when it sees it.
     */
    state->valid = true;
    state->status_valid = true;
    state->lpa = lpa;
    rtl8168_phy_note_bmcr(state, bmcr);
    ++state->refreshes;
    /*
     * ITEM-591/592: the advertisements are read on every refresh too, so the
     * record names what the part answers now - a PHY that reset itself comes
     * back with its own defaults and the difference is visible.  The value is
     * never carried over from an earlier read: a refresh that cannot read the
     * register reports no value and keeps the first error.
     */
    state->advertise = 0U;
    state->advertise_valid = false;
    if (rtl8168_phy_read(io, RTL8168_PHY_REG_ADVERTISE, &advertise) ==
        RTL8168_OK) {
        state->advertise = advertise;
        state->advertise_valid = true;
    } else {
        rtl8168_phy_note_access_first_error(io, state);
    }
    if (state->gigabit) {
        state->advertise_1000 = 0U;
        state->advertise_1000_valid = false;
        if (rtl8168_phy_read(io, RTL8168_PHY_REG_CTRL1000,
                             &advertise_1000) == RTL8168_OK) {
            state->advertise_1000 = advertise_1000;
            state->advertise_1000_valid = true;
        } else {
            rtl8168_phy_note_access_first_error(io, state);
        }
        state->lpa_1000 = 0U;
        state->lpa_1000_valid = false;
        if (rtl8168_phy_read(io, RTL8168_PHY_REG_STAT1000, &lpa_1000) ==
            RTL8168_OK) {
            state->lpa_1000 = lpa_1000;
            state->lpa_1000_valid = true;
        } else {
            rtl8168_phy_note_access_first_error(io, state);
        }
    }
    /* ITEM-592: the vendor status byte, kept beside the MII reading. */
    if (rtl8168_read8(io, RTL8168_REG_PHY_STATUS, &status_byte)) {
        state->phy_status_latest = status_byte;
        state->phy_status_latest_valid = true;
    }
    return RTL8168_OK;
}

/*
 * ITEM-435: the receiver's own arming state.
 *
 * `rtl8168_bring_up()` writes the receive descriptor base and the enable bits
 * and verifies the enable bits by reading them back; it never verified that
 * the part accepted the descriptor base.  A part that answers every other
 * register but keeps a receive descriptor base of its own would then receive
 * into memory the driver does not own, and the driver would poll a ring that
 * stays owned by the device forever - the 2026-09-17 log's shape exactly
 * (`rx_ok=0 rx_err=0` with `tx_recl` advancing).
 *
 * The check is a read-back of the two descriptor-base registers the driver
 * programmed plus the receiver-enable bit, and it is deliberately not fatal:
 * a machine whose transmitter still works must keep publishing its device
 * state, so the caller records the answer instead of tearing the bind down.
 */
enum rtl8168_status rtl8168_rx_armed(
    struct rtl8168_mmio_io *io,
    uint64_t rx_descriptor_physical,
    bool *armed)
{
    uint32_t high;
    uint32_t low;
    uint8_t command;

    if (io == NULL || armed == NULL) {
        return RTL8168_NULL;
    }
    *armed = false;
    if (!rtl8168_read32(io, RTL8168_REG_RX_DESC_HIGH, &high) ||
        !rtl8168_read32(io, RTL8168_REG_RX_DESC_LOW, &low) ||
        !rtl8168_read8(io, RTL8168_REG_CHIP_CMD, &command)) {
        return RTL8168_IO_ERROR;
    }
    *armed =
        ((uint64_t)high << 32U | (uint64_t)low) == rx_descriptor_physical &&
        (command & (uint8_t)RTL8168_CHIP_CMD_RX_ENABLE) != 0U;
    return RTL8168_OK;
}

/*
 * ITEM-643: read-only receive-path snapshot.
 *
 * Every access here is a read.  In particular IntrStatus is read but never
 * acknowledged, so taking the snapshot cannot clear a pending interrupt or
 * race the driver's poll loop.  The descriptor scan only reads the DMA ring
 * the driver already owns and is bounded by its capacity.
 */
enum rtl8168_status rtl8168_rx_diagnostic_read(
    struct rtl8168_mmio_io *io,
    const struct rtl8168_ring *ring,
    struct rtl8168_rx_diagnostic *out)
{
    uint8_t chip_cmd;
    uint16_t intr_status;
    uint16_t intr_mask;
    uint32_t value;

    if (out == NULL) {
        return RTL8168_NULL;
    }
    memset(out, 0, sizeof(*out));
    out->reason = "NULL";
    if (io == NULL) {
        return RTL8168_NULL;
    }
    out->reason = "READ_FAILED";
    if (!rtl8168_read8(io, RTL8168_REG_CHIP_CMD, &chip_cmd) ||
        !rtl8168_read16(io, RTL8168_REG_INTR_STATUS, &intr_status) ||
        !rtl8168_read16(io, RTL8168_REG_INTR_MASK, &intr_mask) ||
        !rtl8168_read32(io, RTL8168_REG_RX_CONFIG, &value)) {
        return RTL8168_IO_ERROR;
    }
    out->chip_cmd = chip_cmd;
    out->intr_status = intr_status;
    out->intr_mask = intr_mask;
    out->rx_config = value;
    if (!rtl8168_read32(io, RTL8168_REG_RX_DESC_LOW, &value)) {
        return RTL8168_IO_ERROR;
    }
    out->rx_desc_low = value;
    if (!rtl8168_read32(io, RTL8168_REG_RX_DESC_HIGH, &value)) {
        return RTL8168_IO_ERROR;
    }
    out->rx_desc_high = value;
    if (!rtl8168_read32(io, RTL8168_REG_MISC, &value)) {
        return RTL8168_IO_ERROR;
    }
    out->misc = value;
    out->available = true;
    out->reason = "OK";
    if (ring == NULL || ring->descriptors == NULL || ring->capacity == 0U) {
        return RTL8168_OK;
    }
    out->rx_index = ring->index;
    out->rx_capacity = ring->capacity;
    out->rx_in_flight = ring->in_flight;
    out->rx_head_opts1 = ring->descriptors[0].opts1;
    out->rx_head_address_low = (uint32_t)ring->descriptors[0].address;
    for (uint32_t index = 0U; index < ring->capacity; ++index) {
        if ((ring->descriptors[index].opts1 & RTL8168_DESC_OWN) != 0U) {
            ++out->rx_own;
        }
    }
    return RTL8168_OK;
}

enum rtl8168_status rtl8168_transmit_poll(struct rtl8168_mmio_io *io)
{
    if (io == NULL) {
        return RTL8168_NULL;
    }
    /* Doorbell: ask the part to poll the normal-priority transmit ring. */
    if (!rtl8168_write8(io, RTL8168_REG_TX_POLL,
                        (uint8_t)RTL8168_TX_POLL_NPQ)) {
        return RTL8168_IO_ERROR;
    }
    return RTL8168_OK;
}

void rtl8168_ring_init(
    struct rtl8168_ring *ring,
    struct rtl8168_descriptor *descriptors,
    uint32_t capacity)
{
    if (ring == NULL) {
        return;
    }
    ring->descriptors = descriptors;
    ring->capacity = descriptors != NULL ? capacity : 0U;
    ring->index = 0U;
    ring->in_flight = 0U;
    ring->submitted = 0U;
    ring->reclaimed = 0U;
    ring->abandoned = 0U;
}

enum rtl8168_status rtl8168_region_calculate(
    struct rtl8168_region_layout *layout)
{
    struct rtl8168_region_layout draft;
    uint32_t rx_descriptor_length;
    uint32_t tx_descriptor_length;
    uint32_t rx_buffer_length;
    uint32_t tx_buffer_length;
    uint32_t offset;

    if (layout == NULL) {
        return RTL8168_NULL;
    }
    if (RTL8168_RX_BUFFER_CAPACITY > RTL8168_RX_BUFFER_SIZE_MAX ||
        RTL8168_TX_BUFFER_CAPACITY < RTL8168_FRAME_MAXIMUM) {
        return RTL8168_CAPACITY;
    }
    rx_descriptor_length =
        RTL8168_RX_DESCRIPTOR_COUNT * RTL8168_DESCRIPTOR_SIZE;
    tx_descriptor_length =
        RTL8168_TX_DESCRIPTOR_COUNT * RTL8168_DESCRIPTOR_SIZE;
    rx_buffer_length =
        RTL8168_RX_DESCRIPTOR_COUNT * RTL8168_RX_BUFFER_STRIDE;
    tx_buffer_length =
        RTL8168_TX_DESCRIPTOR_COUNT * RTL8168_TX_BUFFER_STRIDE;
    if (rx_descriptor_length > RTL8168_DESCRIPTOR_ALIGNMENT) {
        return RTL8168_BAD_STATE;
    }

    memset(&draft, 0, sizeof(draft));
    draft.rx_descriptor_offset = 0U;
    draft.tx_descriptor_offset = RTL8168_DESCRIPTOR_ALIGNMENT;
    offset = draft.tx_descriptor_offset + tx_descriptor_length;
    /* Keep the frame buffers page aligned so the adapter can slice them. */
    offset = (offset + 4095U) & ~4095U;
    draft.rx_buffer_offset = offset;
    offset += rx_buffer_length;
    draft.tx_buffer_offset = offset;
    offset += tx_buffer_length;
    draft.length = offset;
    *layout = draft;
    return RTL8168_OK;
}

uint32_t rtl8168_ring_next(const struct rtl8168_ring *ring, uint32_t index)
{
    if (ring == NULL || ring->capacity == 0U) {
        return 0U;
    }
    return (index + 1U) % ring->capacity;
}

enum rtl8168_status rtl8168_rx_post(
    struct rtl8168_ring *ring,
    uint64_t buffer_physical,
    uint32_t buffer_capacity)
{
    uint32_t index;

    if (ring == NULL || ring->descriptors == NULL) {
        return RTL8168_NULL;
    }
    if (buffer_capacity == 0U ||
        buffer_capacity > RTL8168_RX_BUFFER_SIZE_MAX ||
        (buffer_capacity % RTL8168_BUFFER_ALIGNMENT) != 0U) {
        return RTL8168_INVALID_LENGTH;
    }
    if ((buffer_physical % RTL8168_BUFFER_ALIGNMENT) != 0U) {
        return RTL8168_BAD_STATE;
    }
    /*
     * The buffer addresses are handed over individually by the caller, which
     * owns the region layout; this fills the whole ring in order.
     */
    for (index = 0U; index < ring->capacity; ++index) {
        struct rtl8168_descriptor *descriptor =
            &ring->descriptors[index];
        uint32_t opts1 = RTL8168_DESC_OWN | buffer_capacity;

        if (index == ring->capacity - 1U) {
            opts1 |= RTL8168_DESC_RING_END;
        }
        descriptor->opts2 = 0U;
        descriptor->address =
            buffer_physical + (uint64_t)index * buffer_capacity;
        rtl8168_barrier();
        descriptor->opts1 = opts1;
    }
    ring->index = 0U;
    return RTL8168_OK;
}

enum rtl8168_status rtl8168_rx_complete(
    const struct rtl8168_ring *ring,
    uint32_t buffer_capacity,
    uint32_t *frame_length)
{
    const struct rtl8168_descriptor *descriptor;
    uint32_t opts1;

    if (ring == NULL || ring->descriptors == NULL ||
        frame_length == NULL || ring->capacity == 0U) {
        return RTL8168_NULL;
    }
    *frame_length = 0U;
    descriptor = &ring->descriptors[ring->index % ring->capacity];
    opts1 = descriptor->opts1;
    if ((opts1 & RTL8168_DESC_OWN) != 0U) {
        return RTL8168_RX_NOT_READY;
    }
    rtl8168_barrier();

    /*
     * rtl_rx(): the receive-error bit invalidates the frame, a frame that is
     * not both first and last fragment is not a whole frame, and the
     * descriptor length (bits 13:0) includes the 4-byte FCS the device wrote
     * after the payload, which is not part of the Ethernet frame the stack
     * sees.
     */
    if ((opts1 & RTL8168_RX_STATUS_RECEIVE_ERROR) != 0U) {
        return RTL8168_RX_DROPPED;
    }
    if ((opts1 & (RTL8168_DESC_FIRST_FRAGMENT |
                  RTL8168_DESC_LAST_FRAGMENT)) !=
        (RTL8168_DESC_FIRST_FRAGMENT | RTL8168_DESC_LAST_FRAGMENT)) {
        return RTL8168_RX_DROPPED;
    }
    *frame_length = opts1 & RTL8168_DESC_LENGTH_MASK;
    if (*frame_length <= RTL8168_FCS_LENGTH) {
        return RTL8168_RX_DROPPED;
    }
    *frame_length -= RTL8168_FCS_LENGTH;
    if (*frame_length > buffer_capacity) {
        return RTL8168_RX_DROPPED;
    }
    if (*frame_length < RTL8168_FRAME_MINIMUM) {
        /* A runt that the device still delivered: not a frame to feed. */
        return RTL8168_RX_DROPPED;
    }
    return RTL8168_OK;
}

enum rtl8168_status rtl8168_rx_repost(
    struct rtl8168_ring *ring,
    uint64_t buffer_physical,
    uint32_t buffer_capacity)
{
    struct rtl8168_descriptor *descriptor;
    uint32_t opts1;

    if (ring == NULL || ring->descriptors == NULL) {
        return RTL8168_NULL;
    }
    if (buffer_capacity == 0U ||
        buffer_capacity > RTL8168_RX_BUFFER_SIZE_MAX ||
        (buffer_capacity % RTL8168_BUFFER_ALIGNMENT) != 0U) {
        return RTL8168_INVALID_LENGTH;
    }
    if ((buffer_physical % RTL8168_BUFFER_ALIGNMENT) != 0U) {
        return RTL8168_BAD_STATE;
    }
    if (ring->capacity == 0U) {
        return RTL8168_BAD_STATE;
    }
    descriptor = &ring->descriptors[ring->index % ring->capacity];
    opts1 = RTL8168_DESC_OWN | buffer_capacity;
    if (ring->index % ring->capacity == ring->capacity - 1U) {
        opts1 |= RTL8168_DESC_RING_END;
    }
    /* rtl8169_mark_to_asic(): opts2 = 0, barrier, then release the descriptor. */
    descriptor->opts2 = 0U;
    descriptor->address = buffer_physical;
    rtl8168_barrier();
    descriptor->opts1 = opts1;
    ring->index = rtl8168_ring_next(ring, ring->index);
    return RTL8168_OK;
}

uint32_t rtl8168_tx_frame_length(size_t length)
{
    /*
     * The device puts exactly the programmed length on the wire, so the
     * descriptor carries the real frame length, never the buffer capacity.
     * A frame shorter than the 60-byte Ethernet minimum is zero padded by the
     * caller and transmitted at the padded length, exactly as the reference
     * driver pads to ETH_ZLEN before it maps the frame (__skb_put_padto()).
     * The part also pads a short frame itself ("The RTL8111B/RTL8168B will
     * automatically pad any packets less than 64 bytes (including 4 bytes
     * CRC) ... The padded data are all 0x00"), so the software padding is
     * belt and braces for the length the descriptor advertises.
     */
    if (length == 0U || length > RTL8168_FRAME_MAXIMUM) {
        return 0U;
    }
    if (length < RTL8168_FRAME_MINIMUM) {
        return RTL8168_FRAME_MINIMUM;
    }
    return (uint32_t)length;
}

enum rtl8168_status rtl8168_tx_program(
    struct rtl8168_ring *ring,
    uint64_t frame_physical,
    uint32_t frame_length,
    uint32_t frame_capacity)
{
    struct rtl8168_descriptor *descriptor;
    uint32_t index;
    uint32_t opts1;

    if (ring == NULL || ring->descriptors == NULL) {
        return RTL8168_NULL;
    }
    if (ring->capacity == 0U) {
        return RTL8168_BAD_STATE;
    }
    /* The length rules: a real frame, inside the frame contract, inside the buffer. */
    if (frame_length < RTL8168_FRAME_MINIMUM ||
        frame_length > RTL8168_FRAME_MAXIMUM ||
        frame_length > frame_capacity) {
        return RTL8168_INVALID_LENGTH;
    }
    if (!rtl8168_dma_address_supported(frame_physical)) {
        return RTL8168_UNSUPPORTED_DMA_ADDRESS;
    }
    if (ring->in_flight >= ring->capacity) {
        return RTL8168_RING_FULL;
    }
    index = ring->index % ring->capacity;
    descriptor = &ring->descriptors[index];
    /*
     * rtl8169_tx_map() + the single-segment submission path: one descriptor
     * carries the frame, so it is both the first and the last fragment, the
     * last ring entry carries the ring-end bit, and the ownership bit is
     * programmed last so the device never observes a half-written descriptor.
     */
    descriptor->opts2 = 0U;
    descriptor->address = frame_physical;
    opts1 = RTL8168_DESC_FIRST_FRAGMENT | RTL8168_DESC_LAST_FRAGMENT |
            frame_length;
    if (index == ring->capacity - 1U) {
        opts1 |= RTL8168_DESC_RING_END;
    }
    rtl8168_barrier();
    descriptor->opts1 = opts1 | RTL8168_DESC_OWN;
    ring->index = rtl8168_ring_next(ring, ring->index);
    ++ring->in_flight;
    ++ring->submitted;
    return RTL8168_OK;
}

enum rtl8168_status rtl8168_tx_reclaim(
    struct rtl8168_ring *ring,
    uint32_t *reclaimed)
{
    uint32_t count = 0U;
    uint32_t index;

    if (ring == NULL || ring->descriptors == NULL || reclaimed == NULL) {
        return RTL8168_NULL;
    }
    *reclaimed = 0U;
    if (ring->capacity == 0U) {
        return RTL8168_BAD_STATE;
    }
    while (ring->in_flight > 0U) {
        const struct rtl8168_descriptor *descriptor;
        uint32_t opts1;

        /* The completion cursor trails the programming cursor by in_flight. */
        index = (ring->index + ring->capacity - ring->in_flight) %
                ring->capacity;
        descriptor = &ring->descriptors[index];
        opts1 = descriptor->opts1;
        if ((opts1 & RTL8168_DESC_OWN) != 0U) {
            break;
        }
        rtl8168_barrier();
        --ring->in_flight;
        ++count;
    }
    *reclaimed = count;
    ring->reclaimed += count;
    return count > 0U ? RTL8168_OK : RTL8168_RING_EMPTY;
}

enum rtl8168_status rtl8168_tx_rewind(
    struct rtl8168_ring *ring,
    uint32_t *abandoned)
{
    uint32_t index;

    if (ring == NULL || ring->descriptors == NULL) {
        return RTL8168_NULL;
    }
    if (ring->capacity == 0U) {
        return RTL8168_BAD_STATE;
    }
    if (abandoned != NULL) {
        *abandoned = 0U;
    }
    /*
     * The caller has reset the part, so the device's own descriptor cursor is
     * back at the ring base and none of the memory it left behind is live.
     * Every descriptor is written back as CPU-owned (OWN clear, no address),
     * with a barrier between the fields the device reads and the release, in
     * the same order the submission path uses in reverse.
     */
    for (index = 0U; index < ring->capacity; ++index) {
        struct rtl8168_descriptor *descriptor = &ring->descriptors[index];

        descriptor->opts2 = 0U;
        descriptor->address = 0U;
        rtl8168_barrier();
        descriptor->opts1 = 0U;
    }
    rtl8168_barrier();
    if (abandoned != NULL) {
        *abandoned = ring->in_flight;
    }
    ring->abandoned += ring->in_flight;
    ring->index = 0U;
    ring->in_flight = 0U;
    return RTL8168_OK;
}

const char *rtl8168_status_string(enum rtl8168_status status)
{
    switch (status) {
    case RTL8168_OK:
        return "RTL8168_OK";
    case RTL8168_NULL:
        return "RTL8168_NULL";
    case RTL8168_IO_ERROR:
        return "RTL8168_IO_ERROR";
    case RTL8168_UNSUPPORTED_DEVICE:
        return "RTL8168_UNSUPPORTED_DEVICE";
    case RTL8168_MISSING_MEMORY_BAR:
        return "RTL8168_MISSING_MEMORY_BAR";
    case RTL8168_BAD_BAR:
        return "RTL8168_BAD_BAR";
    case RTL8168_BAD_STATE:
        return "RTL8168_BAD_STATE";
    case RTL8168_INVALID_MAC:
        return "RTL8168_INVALID_MAC";
    case RTL8168_RESET_TIMEOUT:
        return "RTL8168_RESET_TIMEOUT";
    case RTL8168_INVALID_LENGTH:
        return "RTL8168_INVALID_LENGTH";
    case RTL8168_RING_FULL:
        return "RTL8168_RING_FULL";
    case RTL8168_RING_EMPTY:
        return "RTL8168_RING_EMPTY";
    case RTL8168_RX_NOT_READY:
        return "RTL8168_RX_NOT_READY";
    case RTL8168_RX_DROPPED:
        return "RTL8168_RX_DROPPED";
    case RTL8168_UNSUPPORTED_DMA_ADDRESS:
        return "RTL8168_UNSUPPORTED_DMA_ADDRESS";
    case RTL8168_CAPACITY:
        return "RTL8168_CAPACITY";
    case RTL8168_ENABLE_FAILED:
        return "RTL8168_ENABLE_FAILED";
    default:
        return NULL;
    }
}
