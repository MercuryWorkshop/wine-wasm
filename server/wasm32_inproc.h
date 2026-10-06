/*
 * wasm32 (gex): wineserver linked into the Wine loader and run as one of
 * its threads (ntdll/unix/loader.c, start_server), so that its MAP_SHARED
 * memory (the user shared data, session objects) is the process's own:
 * separate wasm memories cannot alias. Force-included into every server
 * object of that build: it renames what the loader also defines, and the
 * server's main and exits.
 */
#define main wineserver_main
#define exit wineserver_exit
#define _exit wineserver_exit
#define get_thread_context wineserver_get_thread_context
#define set_thread_context wineserver_set_thread_context
#define native_machine wineserver_native_machine
#define server_start_time wineserver_server_start_time
#define supported_machines wineserver_supported_machines
#define supported_machines_count wineserver_supported_machines_count
#define user_shared_data wineserver_user_shared_data
/* the process's children are its own (ntdll.so waits for them): the server
 * takes no SIGCHLD, which it would answer with waitpid(-1) */
#define sigaction(sig, act, old) wineserver_sigaction( sig, act, old )  /* (signal.h's prototype, renamed) */
/* the server runs in the foreground here (-f): it never daemonizes, and a
 * fork reference would make the whole loader fork-instrumented (asyncify) */
#define fork wineserver_no_fork  /* (unistd.h's prototype, renamed) */
