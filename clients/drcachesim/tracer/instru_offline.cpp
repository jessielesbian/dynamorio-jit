/* **********************************************************
 * Copyright (c) 2016-2017 Google, Inc.  All rights reserved.
 * **********************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of Google, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/* instru_offline: inserts instrumentation for offline traces.
 */

#include "dr_api.h"
#include "drreg.h"
#include "drutil.h"
#include "drcovlib.h"
#include "instru.h"
#include "../common/trace_entry.h"
#include <limits.h> /* for USHRT_MAX */
#include <stddef.h> /* for offsetof */
#include <string.h> /* for strlen */

static const ptr_uint_t MAX_INSTR_COUNT = 64*1024;

offline_instru_t::offline_instru_t(void (*insert_load_buf)(void *, instrlist_t *,
                                                           instr_t *, reg_id_t),
                                   ssize_t (*write_file)(file_t file,
                                                         const void *data,
                                                         size_t count),
                                   file_t module_file)
    : instru_t(insert_load_buf), write_file_func(write_file), modfile(module_file)
{
    drcovlib_status_t res = drmodtrack_init();
    DR_ASSERT(res == DRCOVLIB_SUCCESS);
    DR_ASSERT(write_file != NULL);
    // Ensure every compiler is packing our struct how we want:
    DR_ASSERT(sizeof(offline_entry_t) == 8);
}

offline_instru_t::~offline_instru_t()
{
    drcovlib_status_t res;
    size_t size = 8192;
    char *buf;
    size_t wrote;
    do {
        buf = (char *)dr_global_alloc(size);
        res = drmodtrack_dump_buf(buf, size, &wrote);
        if (res == DRCOVLIB_SUCCESS) {
            ssize_t written = write_file_func(modfile, buf, wrote - 1/*no null*/);
            DR_ASSERT(written == (ssize_t)strlen(buf));
        }
        dr_global_free(buf, size);
        size *= 2;
    } while (res == DRCOVLIB_ERROR_BUF_TOO_SMALL);
    res = drmodtrack_exit();
    DR_ASSERT(res == DRCOVLIB_SUCCESS);
}

bool
offline_instru_t::custom_module_data
(void * (*load_cb)(module_data_t *module),
 int (*print_cb)(void *data, char *dst, size_t max_len),
 void (*free_cb)(void *data))
{
    drcovlib_status_t res = drmodtrack_add_custom_data(load_cb, print_cb, NULL, free_cb);
    return res == DRCOVLIB_SUCCESS;
}

size_t
offline_instru_t::sizeof_entry() const
{
    return sizeof(offline_entry_t);
}

trace_type_t
offline_instru_t::get_entry_type(byte *buf_ptr) const
{
    offline_entry_t *entry = (offline_entry_t *) buf_ptr;
    switch (entry->addr.type) {
    case OFFLINE_TYPE_MEMREF: return TRACE_TYPE_READ;
    case OFFLINE_TYPE_MEMREF_HIGH: return TRACE_TYPE_READ;
    case OFFLINE_TYPE_PC: return TRACE_TYPE_INSTR;
    case OFFLINE_TYPE_THREAD: return TRACE_TYPE_THREAD;
    case OFFLINE_TYPE_PID: return TRACE_TYPE_PID;
    case OFFLINE_TYPE_TIMESTAMP: return TRACE_TYPE_THREAD; // Closest.
    case OFFLINE_TYPE_IFLUSH: return TRACE_TYPE_INSTR_FLUSH;
    }
    DR_ASSERT(false);
    return TRACE_TYPE_THREAD_EXIT; // Unknown: returning rarest entry.
}

size_t
offline_instru_t::get_entry_size(byte *buf_ptr) const
{
    // We don't know it: the post-processor adds it.
    return 0;
}

addr_t
offline_instru_t::get_entry_addr(byte *buf_ptr) const
{
    offline_entry_t *entry = (offline_entry_t *) buf_ptr;
    return entry->addr.addr;
}

void
offline_instru_t::set_entry_addr(byte *buf_ptr, addr_t addr)
{
    offline_entry_t *entry = (offline_entry_t *) buf_ptr;
    entry->addr.addr = addr;
}

int
offline_instru_t::append_pid(byte *buf_ptr, process_id_t pid)
{
    offline_entry_t *entry = (offline_entry_t *) buf_ptr;
    entry->pid.type = OFFLINE_TYPE_PID;
    entry->pid.pid = pid;
    return sizeof(offline_entry_t);
}

int
offline_instru_t::append_tid(byte *buf_ptr, thread_id_t tid)
{
    offline_entry_t *entry = (offline_entry_t *) buf_ptr;
    entry->tid.type = OFFLINE_TYPE_THREAD;
    entry->tid.tid = tid;
    return sizeof(offline_entry_t);
}

int
offline_instru_t::append_thread_exit(byte *buf_ptr, thread_id_t tid)
{
    offline_entry_t *entry = (offline_entry_t *) buf_ptr;
    entry->extended.type = OFFLINE_TYPE_EXTENDED;
    entry->extended.ext = OFFLINE_EXT_TYPE_FOOTER;
    entry->extended.value = 0;
    return sizeof(offline_entry_t);
}

int
offline_instru_t::append_iflush(byte *buf_ptr, addr_t start, size_t size)
{
    offline_entry_t *entry = (offline_entry_t *) buf_ptr;
    entry->addr.type = OFFLINE_TYPE_IFLUSH;
    entry->addr.addr = start;
    ++entry;
    entry->addr.type = OFFLINE_TYPE_IFLUSH;
    entry->addr.addr = start + size;
    return 2 * sizeof(offline_entry_t);
}

int
offline_instru_t::append_thread_header(byte *buf_ptr, thread_id_t tid)
{
    offline_entry_t *entry = (offline_entry_t *) buf_ptr;
    entry->extended.type = OFFLINE_TYPE_EXTENDED;
    entry->extended.ext = OFFLINE_EXT_TYPE_HEADER;
    entry->extended.value = OFFLINE_FILE_VERSION;
    return sizeof(offline_entry_t) +
        append_unit_header(buf_ptr + sizeof(offline_entry_t), tid);
}

int
offline_instru_t::append_unit_header(byte *buf_ptr, thread_id_t tid)
{
    offline_entry_t *entry = (offline_entry_t *) buf_ptr;
    entry->timestamp.type = OFFLINE_TYPE_TIMESTAMP;
    // We use dr_get_microseconds() for a simple, cross-platform implementation.
    // This is once per buffer write, so a syscall here should be ok.
    // If we want something faster we can try to use the VDSO gettimeofday (via
    // libc) or KUSER_SHARED_DATA on Windows.
    entry->timestamp.usec = dr_get_microseconds();
    return sizeof(offline_entry_t);
}

void
offline_instru_t::insert_save_pc(void *drcontext, instrlist_t *ilist, instr_t *where,
                                 reg_id_t reg_ptr, reg_id_t scratch, int adjust,
                                 uint64_t value)
{
    int disp = adjust;
#ifdef X64
    instrlist_insert_mov_immed_ptrsz(drcontext, (ptr_int_t) value,
                                     opnd_create_reg(scratch),
                                     ilist, where, NULL, NULL);
    MINSERT(ilist, where,
            XINST_CREATE_store(drcontext, OPND_CREATE_MEMPTR(reg_ptr, disp),
                               opnd_create_reg(scratch)));
#else
    instrlist_insert_mov_immed_ptrsz(drcontext, (int)value,
                                     opnd_create_reg(scratch),
                                     ilist, where, NULL, NULL);
    MINSERT(ilist, where,
            XINST_CREATE_store(drcontext, OPND_CREATE_MEMPTR(reg_ptr, disp),
                               opnd_create_reg(scratch)));
    instrlist_insert_mov_immed_ptrsz(drcontext, (int)(value >> 32),
                                     opnd_create_reg(scratch),
                                     ilist, where, NULL, NULL);
    MINSERT(ilist, where,
            XINST_CREATE_store(drcontext, OPND_CREATE_MEMPTR(reg_ptr, disp + 4),
                               opnd_create_reg(scratch)));
#endif
}

void
offline_instru_t::insert_save_addr(void *drcontext, instrlist_t *ilist, instr_t *where,
                                   reg_id_t reg_ptr, reg_id_t reg_addr, int adjust,
                                   opnd_t ref)
{
    bool ok;
    int disp = adjust;
    if (opnd_uses_reg(ref, reg_ptr))
        drreg_get_app_value(drcontext, ilist, where, reg_ptr, reg_ptr);
    if (opnd_uses_reg(ref, reg_addr))
        drreg_get_app_value(drcontext, ilist, where, reg_addr, reg_addr);
    // We use reg_ptr as scratch to get the address.
    ok = drutil_insert_get_mem_addr(drcontext, ilist, where, ref, reg_addr, reg_ptr);
    DR_ASSERT(ok);
    // drutil_insert_get_mem_addr may clobber reg_ptr, so we need to re-load reg_ptr.
    // XXX i#2001: determine whether we have to and avoid it when we don't.
    insert_load_buf_ptr(drcontext, ilist, where, reg_ptr);
    MINSERT(ilist, where,
            XINST_CREATE_store(drcontext,
                               OPND_CREATE_MEMPTR(reg_ptr, disp),
                               opnd_create_reg(reg_addr)));
    // We allow either 0 or all 1's as the type so no need to write anything else.
}

int
offline_instru_t::instrument_memref(void *drcontext, instrlist_t *ilist, instr_t *where,
                                    reg_id_t reg_ptr, reg_id_t reg_tmp, int adjust,
                                    opnd_t ref, bool write, dr_pred_type_t pred)
{
    // Post-processor distinguishes read, write, prefetch, flush, and finds size.
    instr_t *label = INSTR_CREATE_label(drcontext);
    MINSERT(ilist, where, label);
    insert_save_addr(drcontext, ilist, where, reg_ptr, reg_tmp, adjust, ref);
#ifdef ARM // X86 does not support general predicated execution
    if (pred != DR_PRED_NONE) {
        instr_t *instr;
        for (instr  = instr_get_prev(where);
             instr != label;
             instr  = instr_get_prev(instr)) {
            DR_ASSERT(!instr_is_predicated(instr));
            instr_set_predicate(instr, pred);
        }
    }
#endif
    return (adjust + sizeof(offline_entry_t));
}

// We stored the instr count in *bb_field in bb_analysis().
int
offline_instru_t::instrument_instr(void *drcontext, void *tag, void **bb_field,
                                   instrlist_t *ilist, instr_t *where,
                                   reg_id_t reg_ptr, reg_id_t reg_tmp, int adjust,
                                   instr_t *app)
{
    app_pc pc, modbase;
    uint modidx;
    offline_entry_t entry;
    // We write just once per bb.
    if ((ptr_uint_t)*bb_field > MAX_INSTR_COUNT)
        return adjust;

    pc = dr_fragment_app_pc(tag);
    if (drmodtrack_lookup(drcontext, pc, &modidx, &modbase) != DRCOVLIB_SUCCESS) {
        // FIXME i#2062: add non-module support.  The plan for instrs is to have
        // one entry w/ the start abs pc, and subsequent entries that pack the instr
        // length for 10 instrs, 4 bits each, into a pc.modoffs field.  We will
        // also need to store the type (read/write/prefetch*) and size for the
        // memrefs.
        modidx = 0;
        modbase = pc;
    }
    entry.pc.type = OFFLINE_TYPE_PC;
    // We put the ARM vs Thumb mode into the modoffs to ensure proper decoding.
    entry.pc.modoffs =
        dr_app_pc_as_jump_target(instr_get_isa_mode(where), pc) - modbase;
    entry.pc.modidx = modidx;
    entry.pc.instr_count = (ptr_uint_t)*bb_field;
    insert_save_pc(drcontext, ilist, where, reg_ptr, reg_tmp, adjust,
                   entry.combined_value);
    *(ptr_uint_t*)bb_field = MAX_INSTR_COUNT + 1;
    return (adjust + sizeof(offline_entry_t));
}

int
offline_instru_t::instrument_ibundle(void *drcontext, instrlist_t *ilist, instr_t *where,
                                     reg_id_t reg_ptr, reg_id_t reg_tmp, int adjust,
                                     instr_t **delay_instrs, int num_delay_instrs)
{
    // The post-processor fills in all instr info other than our once-per-bb entry.
    return adjust;
}

void
offline_instru_t::bb_analysis(void *drcontext, void *tag, void **bb_field,
                             instrlist_t *ilist, bool repstr_expanded)
{
    instr_t *instr;
    ptr_uint_t count = 0;
    app_pc last_xl8 = NULL;
    if (repstr_expanded) {
        // The same-translation check below is not sufficient as drutil uses
        // two different translations to deal with complexities.
        // Thus we hardcode this.
        count = 1;
    } else {
        for (instr = instrlist_first_app(ilist); instr != NULL;
             instr = instr_get_next_app(instr)) {
            // To deal with app2app changes, we do not double-count consecutive instrs
            // with the same translation.
            if (instr_get_app_pc(instr) != last_xl8)
                ++count;
            last_xl8 = instr_get_app_pc(instr);
        }
    }
    *bb_field = (void *)count;
}
