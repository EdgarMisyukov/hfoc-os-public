/* SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-HFOC-Commercial */
#ifndef TEST_RTL8168_PHY_MODEL_H
#define TEST_RTL8168_PHY_MODEL_H

/* Register storage, not a second initialization algorithm. The independently
 * specified write expectations live in rtl8168-host.c; see the v6.12 source
 * references and checksums in docs/rtl8168evl-phy-audit.md. */
struct phy_model_write {
    uint16_t page, selector, reg, value;
};

struct phy_model {
    uint16_t pages[8][32];
    uint16_t extended[256][32];
    uint16_t parameters[65536];
    uint16_t ext_selector;
    uint16_t param_selector;
    uint16_t corner;
    struct phy_model_write writes[128];
    unsigned write_count;
    unsigned reset_requests;
    uint64_t reset_until;
    uint64_t reset_finished;
    bool reset_stuck;
    /* Toshiba Linux atomic probe: 1f reads0 while the bank really changes.
     * This affects only readback, never the actual register address. */
    bool page_readback_zero;
    uint16_t nonzero_page_readback;
    unsigned bad_pages;
    bool fault_enabled;
    bool fault_fired;
    bool fault_write;
    bool fault_restore;
    unsigned fault_after_writes;
    uint16_t fault_page, fault_selector, fault_reg;
};

static void phy_model_init(struct phy_model *model)
{
    memset(model, 0, sizeof(*model));
    model->extended[0x2d][0x18] = 0x0120U;
    model->extended[0x20][0x15] = 0x0420U;
    model->parameters[0x8b85] = 0x0012U;
    model->parameters[0x8b86] = 0x0200U;
    model->corner = 0x0080U;
    model->pages[3][0x19] = 0x0200U;
    model->pages[3][0x10] = 0x0100U;
    model->pages[5][1] = 0x0002U;
}

static uint16_t phy_model_selector(const struct phy_model *model,
                                    uint16_t page)
{
    return page == 7U ? model->ext_selector :
           page == 5U ? model->param_selector : 0U;
}

static bool phy_model_accept(struct phy_model *model, uint16_t page,
                             uint16_t reg, bool write, uint16_t value)
{
    if (model->fault_restore && model->fault_fired && write &&
        reg == 0x1fU && value == 0U) {
        return false;
    }
    if (model->fault_enabled && !model->fault_fired &&
        model->write_count >= model->fault_after_writes &&
        page == model->fault_page && reg == model->fault_reg &&
        write == model->fault_write &&
        phy_model_selector(model, page) == model->fault_selector) {
        model->fault_fired = true;
        return false;
    }
    return true;
}

static uint16_t *phy_model_register(struct phy_model *model, uint16_t *standard,
                                     uint16_t page, uint16_t reg)
{
    if (page == 0U) {
        return &standard[reg];
    }
    if (page == 7U && model->ext_selector < 256U) {
        return reg == 0x1eU ? &model->ext_selector :
                             &model->extended[model->ext_selector][reg];
    }
    if (page == 5U) {
        if (reg == 5U) {
            return &model->param_selector;
        }
        if (reg == 6U) {
            return &model->parameters[model->param_selector];
        }
        if (reg == 0x17U && model->param_selector == 0x8b80U) {
            return &model->corner;
        }
    }
    if (page < 8U) {
        return &model->pages[page][reg];
    }
    ++model->bad_pages;
    return &model->pages[0][reg];
}

static uint16_t phy_model_read(struct phy_model *model, uint16_t *standard,
                                uint16_t page, uint16_t reg, uint64_t now)
{
    if (reg == 0x1fU) {
        return page != 0U && model->nonzero_page_readback != 0U ?
                   model->nonzero_page_readback :
               model->page_readback_zero ? 0U : page;
    }
    if (page == 0U && reg == 0U && model->reset_requests != 0U &&
        model->reset_finished == 0U && !model->reset_stuck &&
        now >= model->reset_until) {
        standard[0] = 0x1140U;
        model->reset_finished = now;
    }
    return *phy_model_register(model, standard, page, reg);
}

static void phy_model_write(struct phy_model *model, uint16_t *standard,
                             uint16_t *page, uint16_t reg, uint16_t value,
                             uint64_t now)
{
    if (reg == 0x1fU) {
        *page = value;
        return;
    }
    if (!(*page == 7U && reg == 0x1eU) &&
        !(*page == 5U && reg == 5U) &&
        model->write_count < 128U) {
        model->writes[model->write_count++] = (struct phy_model_write){
            *page, phy_model_selector(model, *page), reg, value
        };
    }
    *phy_model_register(model, standard, *page, reg) = value;
    if (*page == 0U && reg == 0U && (value & 0x8000U) != 0U) {
        ++model->reset_requests;
        model->reset_until = now + 150000U;
        model->reset_finished = 0U;
        /* Control defaults change on reset, vendor parameter storage stays.
         * Advertisements must therefore be programmed AFTER reset finishes. */
        standard[4] = 0U;
        standard[9] = 0U;
    }
}
#endif
