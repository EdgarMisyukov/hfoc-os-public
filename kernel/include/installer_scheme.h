#ifndef HFOC_INSTALLER_SCHEME_H
#define HFOC_INSTALLER_SCHEME_H

/*
 * ITEM-514: the install scheme, as a value both the storage model and the
 * install flow name.
 *
 * The enumeration lives in its own header with no includes because two very
 * different translation units need it: the storage model/screen (which pulls
 * the display abstraction through `installer_ui.h`) and the install flow
 * (which is included by `console.c`, where `display.h`'s accessors collide
 * with console.c's own static names).  A shared value must not drag a screen
 * header into the console.
 */
enum installer_storage_scheme {
    INSTALLER_STORAGE_SCHEME_NONE = 0,
    INSTALLER_STORAGE_SCHEME_ALONGSIDE = 1,
    INSTALLER_STORAGE_SCHEME_FREE_SPACE = 2,
    INSTALLER_STORAGE_SCHEME_ERASE = 3,
    INSTALLER_STORAGE_SCHEME_MANUAL = 4,
};

/* The four schemes above, in the order the screen lists them. */
#define INSTALLER_STORAGE_SCHEMES_MAX 4U

#endif
