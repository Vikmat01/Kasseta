typedef void (*fn)(int, char**, char**);
extern fn __init_array_start[] __attribute__((weak, visibility("hidden")));
extern fn __init_array_end[] __attribute__((weak, visibility("hidden")));
void __my_csu_init(int argc, char **argv, char **envp) {
	unsigned long n = __init_array_end - __init_array_start;
	for (unsigned long i = 0; i < n; i++) __init_array_start[i](argc, argv, envp);
}
void *__dso_handle __attribute__((visibility("hidden"))) = &__dso_handle;
