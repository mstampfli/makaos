/*
 * tlsdso -- a shared object with a __thread variable, for the dynamic-TLS
 * (Phase 4) test.  Built with `ld -shared`, its __thread access uses the
 * general-dynamic model: an import of __tls_get_addr + a DTPMOD64 reloc the
 * loader must register as a TLS module.  Each thread that touches t_counter
 * must get its OWN copy, initialised to the template value 7.
 */
static __thread int t_counter = 7;

int  dso_tls_get(void)  { return t_counter; }
void dso_tls_set(int v) { t_counter = v; }
