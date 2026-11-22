/* Android boot diagnostic derived from TSFP PC's native entry, GPL-3.0-only. */
#include <xbox/xboxrecomp.h>
#include "xbox_watchpoint.h"
#include "vr_options.h"
#include "xr.h"

#include <SDL3/SDL.h>
#ifndef TSFP_RELEASE
#include <android/log.h>
#endif
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern RECOMP_TLS uint32_t g_esp;
extern ptrdiff_t g_xbox_mem_offset;
extern void xbe_entry_point(void);
extern int recomp_dispatch_init(void);
extern void xbox_path_init(const char *game, const char *saves);
extern void xbox_SetApuInterruptSource(int (*pending)(void));
extern int tsfp_android_mmio(siginfo_t *info, void *context);

static struct sigaction original_bus;
static struct sigaction original_segv;

static void memory_fault(int sig, siginfo_t *info, void *context)
{
    if (!tsfp_android_mmio(info, context))
        sigaction(sig, sig == SIGBUS ? &original_bus : &original_segv, NULL);
}

static int boot_game(const char *game, const char *saves)
{
    char path[4096];
    if (snprintf(path, sizeof path, "%s/default.xbe", game) >= (int)sizeof path)
        return 2;
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror(path);
        return 2;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 2;
    }
    long size = ftell(file);
    if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 2;
    }
    void *xbe = malloc((size_t)size);
    if (!xbe || fread(xbe, 1, (size_t)size, file) != (size_t)size) {
        fclose(file);
        free(xbe);
        return 2;
    }
    fclose(file);
    printf("[TSFP] Loaded default.xbe (%ld bytes)\n", size);

    struct sigaction handler = {0};
    handler.sa_sigaction = memory_fault;
    handler.sa_flags = SA_SIGINFO;
    sigemptyset(&handler.sa_mask);
    if (sigaction(SIGBUS, &handler, &original_bus) != 0 ||
        sigaction(SIGSEGV, &handler, &original_segv) != 0) {
        perror("sigaction");
        free(xbe);
        return 1;
    }
    if (!xbox_MemoryLayoutInit(xbe, (size_t)size)) {
        fprintf(stderr, "[TSFP] Xbox memory initialization failed\n");
        free(xbe);
        return 1;
    }
    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("[TSFP] Xbox memory mapped at %p\n", (void *)g_xbox_mem_offset);
    xbox_kernel_init();
    xbox_path_init(game, saves);
    xbox_kernel_bridge_init();
    xbox_watch_init();
    if (xbox_EnvSwitch("RECOMP_AC97_READY", 1)) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)g_xbox_mem_offset + XBOX_CONTIG_BASE);
        if (g_apu_state)
            xbox_SetApuInterruptSource(mcpx_apu_irq_pending);
    }
    g_esp = XBOX_STACK_TOP;
    if (!recomp_dispatch_init())
        fprintf(stderr, "[TSFP] Dispatch uses binary search\n");
    xbox_WatchdogStart();
    printf("[TSFP] Entering translated Xbox entry\n");
    xbe_entry_point();
    printf("[TSFP] Guest entry returned\n");
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe);
    return 0;
}

#ifndef TSFP_RELEASE
static void load_test_input(const char *cache)
{
    char path[4096];
    if (snprintf(path, sizeof path, "%s/input-sequence.txt", cache) >= (int)sizeof path)
        return;
    FILE *file = fopen(path, "r");
    if (!file)
        return;
    char sequence[4096];
    if (fgets(sequence, sizeof sequence, file)) {
        size_t length = strcspn(sequence, "\r\n");
        if (length < sizeof sequence - 1) {
            sequence[length] = '\0';
            setenv("RECOMP_INPUT_SEQ", sequence, 1);
        }
    }
    fclose(file);
}
#endif

__attribute__((visibility("default")))
int tsfp_android_main(int argc, char **argv)
{
    if (argc != 14)
        return 2;
#ifdef TSFP_RELEASE
    int log_fd = open("/dev/null", O_WRONLY);
#else
    int log_fd = open(argv[4], O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0600);
#endif
    if (log_fd < 0)
        return 2;
    int redirected = dup2(log_fd, STDOUT_FILENO) >= 0 && dup2(log_fd, STDERR_FILENO) >= 0;
    if (log_fd > STDERR_FILENO)
        close(log_fd);
    if (!redirected)
        return 2;

#ifndef TSFP_RELEASE
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
#endif
    if (strcmp(argv[6], "openxr") == 0) {
        tsfp_xr_enable(argv[5], argv[8], argv[3], argv[13],
                       strcmp(argv[10], "2") == 0 ? 2 : 1, strcmp(argv[11], "4") == 0 ? 4 : 1);
        tsfp_vr_options_init(argv[12], argv[9]);
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "[TSFP] SDL initialization failed: %s\n", SDL_GetError());
        return 1;
    }
    setenv("RECOMP_GAME_DIR", argv[1], 1);
    setenv("RECOMP_SAVE_DIR", argv[2], 1);
    setenv("TMPDIR", argv[3], 1);
    setenv("XDG_CONFIG_HOME", argv[3], 1);
#ifdef TSFP_RELEASE
    setenv("RECOMP_TRACE_BUDGET", "0", 1);
    setenv("RECOMP_EXIT_TRACE", "0", 1);
#else
    setenv("RECOMP_TRACE_BUDGET", *argv[5] || *argv[7] ? "500" : "0", 0);
    load_test_input(argv[3]);
    if (*argv[7]) {
        setenv("TSFP_FRAME_PROFILE", argv[7], 1);
        setenv("TSFP_RHI_UPLOAD_PROFILE", "1", 1);
    }
    if (*argv[5]) {
        setenv("RECOMP_HLE_D3D8_DUMP", argv[5], 1);
        setenv("RECOMP_HLE_D3D8_DUMP_EVERY", "60", 1);
        setenv("RECOMP_HLE_D3D8_DUMP_KEEP_LAST", "2", 1);
    }
    __android_log_write(ANDROID_LOG_INFO, "TSFP", "Starting Xbox guest initialization");
#endif
    int result = boot_game(argv[1], argv[2]);
    SDL_Quit();
#ifndef TSFP_RELEASE
    __android_log_print(ANDROID_LOG_INFO, "TSFP", "Xbox guest returned %d", result);
#endif
    return result;
}
