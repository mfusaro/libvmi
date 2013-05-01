/* The LibVMI Library is an introspection library that simplifies access to
 * memory in a target virtual machine or in a file containing a dump of
 * a system's physical memory.  LibVMI is based on the XenAccess Library.
 *
 * Copyright 2011 Sandia Corporation. Under the terms of Contract
 * DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government
 * retains certain rights in this software.
 *
 * Author: Nasser Salim (njsalim@sandia.gov)
 * Author: Steven Maresca (steven.maresca@zentific.com)
 * Author: Tamas K Lengyel (tamas.lengyel@zentific.com)
 *
 * This file is part of LibVMI.
 *
 * LibVMI is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * LibVMI is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with LibVMI.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "libvmi.h"
#include "private.h"
#include "driver/interface.h"

#define _GNU_SOURCE
#include <glib.h>

//----------------------------------------------------------------------------
//  General event callback management.

gboolean event_entry_free (gpointer key, gpointer value, gpointer data)
{
    vmi_instance_t vmi=(vmi_instance_t)data;
    vmi_event_t *event = (vmi_event_t*)value;
    vmi_clear_event(vmi, event);
    return TRUE;
}

void events_init (vmi_instance_t vmi)
{
    if(!(vmi->init_mode & VMI_INIT_EVENTS)){
        return;
    }

    vmi->mem_events = g_hash_table_new(g_int64_hash, g_int64_equal);
    vmi->reg_events = g_hash_table_new(g_int_hash, g_int_equal);
}

void events_destroy (vmi_instance_t vmi)
{
    if(!(vmi->init_mode & VMI_INIT_EVENTS)){
        return;
    }

    g_hash_table_foreach_steal(vmi->mem_events, event_entry_free, vmi);
    g_hash_table_foreach_steal(vmi->reg_events, event_entry_free, vmi);

    g_hash_table_destroy(vmi->mem_events);
    g_hash_table_destroy(vmi->reg_events);
}

//----------------------------------------------------------------------------
// Public event functions.

vmi_event_t *vmi_get_reg_event (vmi_instance_t vmi,
                              registers_t reg) {
    return g_hash_table_lookup(vmi->reg_events, &reg);
}

vmi_event_t *vmi_get_mem_event (vmi_instance_t vmi,
                              addr_t page) {
    return g_hash_table_lookup(vmi->mem_events, &page);
}

status_t vmi_register_event (vmi_instance_t vmi,
                           vmi_event_t* event)
{
    status_t rc = VMI_FAILURE;

    if(!(vmi->init_mode & VMI_INIT_EVENTS)){
        dbprint("LibVMI wasn't initialized with events!\n");
        return VMI_FAILURE;
    }
    if(!event) {
        dbprint("No event given!\n");
        return VMI_FAILURE;
    }
    if(!event->callback) {
        dbprint("No event callback function specified!\n");
        return VMI_FAILURE;
    }

    switch(event->type){
        case VMI_EVENT_REGISTER:
            if(NULL!=g_hash_table_lookup(vmi->reg_events, &(event->reg_event.reg))) {
                dbprint("An event is already registered on this reg: %d\n",
                    event->reg_event.reg);
            } else {
                if(VMI_SUCCESS == driver_set_reg_access(vmi, event->reg_event)){
                    g_hash_table_insert(vmi->reg_events, &(event->reg_event.reg), event);
                    dbprint("Enabled register event on reg: %d\n",
                        event->reg_event.reg);
                    rc = VMI_SUCCESS;
                }
            }

            break;
        case VMI_EVENT_MEMORY:
            if(NULL!=g_hash_table_lookup(vmi->mem_events, &(event->mem_event.page))) {
                dbprint("An event is already registered on this page: %"PRIu64"\n",
                    event->mem_event.page);
            } else {
                if(VMI_SUCCESS == driver_set_mem_access(vmi, event->mem_event)){
                    g_hash_table_insert(vmi->mem_events, &(event->mem_event.page), event);

                    dbprint("Enabling memory event on pages: %"PRIu64" + %"PRIu64"\n",
                        event->mem_event.page, event->mem_event.npages);
                    rc = VMI_SUCCESS;
                }
            }

            break;
        case VMI_EVENT_SINGLESTEP:
            if(VMI_SUCCESS == driver_start_single_step(vmi, event->ss_event)){
                    #warning vcpu? or JUST that vcpu? probably need loop here for EACH vcpu
                    g_hash_table_insert(vmi->mem_events, &(event->ss_event.vcpus), event);

                    dbprint("Enabling single step\n");
                    rc = VMI_SUCCESS;
            }

            break;
        default:
            errprint("Unknown event type: %d\n", event->type);
    }

    return rc;
}

status_t vmi_clear_event (vmi_instance_t vmi,
                          vmi_event_t* event)
{
    status_t rc = VMI_FAILURE;

    if(!(vmi->init_mode & VMI_INIT_EVENTS)){
        return VMI_FAILURE;
    }

    switch(event->type) {
       case VMI_EVENT_SINGLESTEP:
            if(event->type == VMI_EVENT_SINGLESTEP){
                if(CHECK_VCPU_SINGLESTEP(event->ss_event, 
                    event->vcpu_id)){
                    dbprint("Disabling single step on vcpu: %lu\n", 
                        event->vcpu_id);
                    rc = driver_stop_single_step(vmi, event->vcpu_id);
                }
#warning do we pull from hashtable here or not. key using event->ss_event?? or what
                if(!vmi->shutting_down && rc==VMI_SUCCESS) {
                    g_hash_table_remove(vmi->ss_events, &(event->ss_event));
                }
            }
            break;
        case VMI_EVENT_REGISTER:
            if(NULL!=g_hash_table_lookup(vmi->reg_events, &(event->reg_event.reg))) {
                dbprint("Disabling register event on reg: %d\n",
                    event->reg_event.reg);
                event->reg_event.in_access = VMI_REG_N;
                rc = driver_set_reg_access(vmi, event->reg_event);
                if(!vmi->shutting_down && rc==VMI_SUCCESS) {
                    g_hash_table_remove(vmi->reg_events, &(event->reg_event.reg));
                }
            }
            break;
        case VMI_EVENT_MEMORY:
            if(NULL!=g_hash_table_lookup(vmi->mem_events, &(event->mem_event.page))) {
                dbprint("Disabling memory event on page: %"PRIu64"\n",
                    event->mem_event.page);
                event->mem_event.in_access = VMI_MEM_N;
                rc = driver_set_mem_access(vmi, event->mem_event);
                if(!vmi->shutting_down && rc==VMI_SUCCESS) {
                    g_hash_table_remove(vmi->mem_events, &(event->mem_event.page));
                }
            }
            break;
        default:
            errprint("Cannot clear unknown event: %d\n", event->type);
            return VMI_FAILURE;
    }

    return rc;
}

status_t vmi_events_listen(vmi_instance_t vmi, uint32_t timeout){

    if(!(vmi->init_mode & VMI_INIT_EVENTS)){
        return VMI_FAILURE;
    }

    return driver_events_listen(vmi, timeout);
}

status_t vmi_stop_single_step_vcpu(vmi_instance_t vmi, unsigned long vcpu){
    
    if(!(vmi->init_mode & VMI_INIT_EVENTS)){
        return VMI_FAILURE;
    }
    
    return xen_stop_single_step(vmi, vcpu);
}

status_t vmi_shutdown_single_step(vmi_instance_t vmi){

    if(!(vmi->init_mode & VMI_INIT_EVENTS)){
        return VMI_FAILURE;
    }

    return driver_shutdown_single_step(vmi);
}
