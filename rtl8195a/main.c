/*
 * This file is part of the Micro Python project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2016 Chester Tseng
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*****************************************************************************
 *                              Header includes
 * ***************************************************************************/

#include "py/mpconfig.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "py/compile.h"
#include "py/gc.h"

#include "lib/utils/pyexec.h"

#include "gccollect.h"
#include "exception.h"

#include "section_config.h"
#include "FreeRTOS.h"
#include "task.h"


/*****************************************************************************
 *                              Internal variables
 * ***************************************************************************/
SECTION(".sdram.bss") uint8_t mpHeap[1024 * 1024];      // MicroPython core' heap 

void micropython_task(void const *arg) {
// get the top of the stack to initialize the garbage collector
    uint32_t sp = gc_helper_get_sp();

#if MICROPY_PY_THREAD
    mp_thread_init();
#endif
    mp_stack_set_top((void*)sp);
#if MICROPY_ENABLE_GC
    gc_init(mpHeap, mpHeap + sizeof(mpHeap));
#endif
    // Init micropython basic system
    mp_init();
    mp_obj_list_init(mp_sys_path, 0);
    mp_obj_list_init(mp_sys_argv, 0);
#if MICROPY_VFS_FAT
    memset(MP_STATE_PORT(fs_user_mount), 0, sizeof(MP_STATE_PORT(fs_user_mount)));
#endif
    MP_STATE_PORT(mp_kbd_exception) = mp_obj_new_exception(&mp_type_KeyboardInterrupt);

    modterm_init();
    modmachine_init();
    modnetwork_init();

    netif_init0();
    wlan_init0();

    pyexec_frozen_module("_boot.py");
    pyexec_file("main.py");
    modterminal_rx_loop();
    vTaskDelete(NULL);
}

/*
 * In FreeRTOS v8.1.2 , TCB is still malloc from ucHeap, instead of independent memory,
 * so it's not quite easy to predict usage.
 */
SECTION(".sdram.bss") uint8_t ucHeap[configTOTAL_HEAP_SIZE];

/* MicroPython Task's stack memory
 * Put this memory to on-board sram to accerlate speed
 * ".bdsram.data" section
 */
SECTION(".bdsram.data") StackType_t mpTaskStack[MICROPY_TASK_STACK_DEPTH];

void main (void) {
    /* 
     * lwip init will use freertos thread, so vPortDefineHeapRegions
     * should be called before lwip init
     */
    HeapRegion_t xHeapRegions[] = {{ ucHeap, sizeof(ucHeap) },{ NULL, 0 }};
    vPortDefineHeapRegions(xHeapRegions);

    // Create MicroPython main task
    TaskHandle_t xReturn = xTaskGenericCreate(micropython_task,
            (signed char *)MICROPY_TASK_NAME, 
            MICROPY_TASK_STACK_DEPTH, 
            NULL,
            MICROPY_TASK_PRIORITY,
            NULL,           // No arguments to pass to mp thread
            mpTaskStack,    // Use user define stack memory to make it predictable.
            NULL);

    if (xReturn != pdTRUE)
        mp_printf(&mp_plat_print, "Create %s task failed", MICROPY_TASK_NAME);
    else
        vTaskStartScheduler();

    for(;;);
    return;
}

mp_import_stat_t mp_import_stat(const char *path) {
#if MICROPY_VFS_FAT
    return fat_vfs_import_stat(path);
#else
    (void)path;
    return MP_IMPORT_STAT_NO_EXIST;
#endif
}

#if !MICROPY_VFS_FAT
mp_lexer_t *mp_lexer_new_from_file(const char *filename) {
    return NULL;
}
#endif

mp_obj_t mp_builtin_open(uint n_args, const mp_obj_t *args, mp_map_t *kwargs) {
#if MICROPY_VFS_FAT
    // TODO: Handle kwargs!
    return vfs_proxy_call(MP_QSTR_open, n_args, args);
#else
    return mp_const_none;
#endif
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, mp_builtin_open);

void nlr_jump_fail(void *val) {
    mp_printf(&mp_plat_print, "FATAL: uncaught exception %p\r\n", val);
    for (uint i = 0;;) {
        for (volatile uint delay = 0; delay < 10000000; delay++);
    }
}