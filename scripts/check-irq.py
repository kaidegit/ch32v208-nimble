#!/usr/bin/env python3
"""Check the production FreeRTOS critical/yield state machine on the host."""
import os
from pathlib import Path
import runpy
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
function = runpy.run_path(str(ROOT / 'scripts/check-phy.py'))['function']
source = (ROOT / 'third_party/freertos/portable/GCC/RISC-V/port.c').read_text()
source = re.sub(r'(?m)^void (vPort(?:EnterCritical|ExitCritical|Yield))', r'void\n\1', source)
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t UBaseType_t;
static UBaseType_t uxCriticalNesting, uxCriticalSavedMask, uxYieldDeferred;
static UBaseType_t g_port_yield_bad_mask, irq_mask = 0x88;
static unsigned requests, switch_during_request;
#define configASSERT(x) assert(x)
#define Software_IRQn 14
void vPortEnterCritical(void);
void vPortExitCritical(void);
UBaseType_t xPortSetInterruptMask(void) {
    UBaseType_t saved = irq_mask;
    irq_mask = 0;
    return saved;
}
void vPortClearInterruptMask(UBaseType_t saved) { irq_mask = saved; }
void NVIC_SetPendingIRQ(unsigned irq) {
    assert(irq == Software_IRQn && uxCriticalNesting == 0);
    ++requests;
    /* A newly scheduled task may replace the global outer saved mask
     * before the yielding task resumes its own restore instruction. */
    if (switch_during_request) {
        irq_mask = 0x88;
        vPortEnterCritical();
        vPortExitCritical();
    }
}
''' + '\n'.join(function(source, name) for name in
                 ['vPortEnterCritical', 'vPortExitCritical', 'vPortYield']) + r'''
int main(void) {
    const unsigned masks[] = {0, 8, 0x80, 0x88};
    for (unsigned i = 0; i < 4; i++) {
        requests = 0;
        irq_mask = masks[i];
        vPortEnterCritical();
        assert(irq_mask == 0 && uxCriticalNesting == 1);
        vPortYield();
        vPortEnterCritical();
        vPortYield();
        vPortYield();
        vPortExitCritical();
        assert(requests == 0 && uxCriticalNesting == 1 && irq_mask == 0);
        switch_during_request = 1;
        vPortExitCritical();
        switch_during_request = 0;
        assert(requests == 1 && uxCriticalNesting == 0 && !uxYieldDeferred);
        assert(irq_mask == masks[i]);
        vPortYield();
        assert(requests == 2 && irq_mask == masks[i]);
    }
    assert(g_port_yield_bad_mask == 0);
    puts("PASS: yield deferred/coalesced until outer exit; original IRQ state preserved across switches");
}
'''
with tempfile.TemporaryDirectory(prefix='wch-irq-check-') as temp:
    path, binary = Path(temp) / 'irq.c', Path(temp) / 'irq'
    path.write_text(harness)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
