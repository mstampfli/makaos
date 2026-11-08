// crtdso.c - supplies __dso_handle for MakaOS shared objects.
//
// gcc emits a hidden reference to __dso_handle for every C++ static object with
// a destructor (via __cxa_atexit), expecting crtbeginS.o to define it. MakaOS
// links shared objects with a bare `ld -shared` and no crt fragments, so this
// object is added to the shared-link startfile spec to provide it. Hidden +
// self-referential, exactly as crtbeginS.o does, so the address is module-local.
__attribute__((visibility("hidden"))) void* __dso_handle = &__dso_handle;
