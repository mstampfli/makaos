/*
 * __dso_handle -- every shared object needs its own: an opaque per-DSO token
 * that __cxa_atexit/__cxa_finalize use to group THIS object's C++ static
 * destructors so dlclose runs only its own.  crtbeginS.o normally supplies it;
 * our raw `ld -shared` links no CRT, so libSDL3.so (and any C++ .so) gets it
 * here.  Hidden, so it stays local to the DSO (matching the hidden reference);
 * its ADDRESS is the token -- the value is never dereferenced.
 */
__attribute__((visibility("hidden"))) void *__dso_handle = 0;
