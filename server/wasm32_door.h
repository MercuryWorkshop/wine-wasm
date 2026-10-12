/*
 * wasm32 (gex): force-included into every wineserver object of the door
 * build (gex docs/wineserver-kernel.md). The server runs in the foreground
 * (-f): it never forks, and a fork reference would make the whole program
 * fork-instrumented (asyncify), which a door instance cannot run.
 */
#define fork wineserver_no_fork  /* (unistd.h's prototype, renamed) */
