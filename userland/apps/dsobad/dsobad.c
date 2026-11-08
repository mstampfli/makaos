/*
 * dsobad -- a .so that imports a STRONG symbol no object provides.  dlopen must
 * REJECT it (return NULL) and clean up (unpublish + munmap), not crash or half-
 * load it.  Reject-path test for the loader's undefined-symbol handling.
 */
extern int this_symbol_does_not_exist_anywhere(int);   /* strong UND -> unresolvable */

int dsobad_entry(void) { return this_symbol_does_not_exist_anywhere(7); }
