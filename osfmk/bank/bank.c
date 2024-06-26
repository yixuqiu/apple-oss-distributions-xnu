/*
 * Copyright (c) 2012-2020 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

#include <bank/bank_internal.h>
#include <bank/bank_types.h>
#include <mach/mach_types.h>
#include <mach/kern_return.h>
#include <ipc/ipc_port.h>
#include <ipc/ipc_voucher.h>
#include <mach/mach_vm.h>
#include <mach/vm_map.h>
#include <vm/vm_map.h>
#include <mach/host_priv.h>
#include <mach/host_special_ports.h>
#include <kern/host.h>
#include <kern/ledger.h>
#include <kern/coalition.h>
#include <kern/thread_group.h>
#include <sys/kdebug.h>
#include <IOKit/IOBSD.h>
#include <kern/policy_internal.h>

/* we can't include the BSD <sys/persona.h> header here... */
#ifndef PERSONA_ID_NONE
#define PERSONA_ID_NONE ((uint32_t)-1)
#endif
/* can't include <sys/kauth.h> either */
#ifndef KAUTH_UID_NONE
#define KAUTH_UID_NONE  (~(uint32_t)0 - 100)
#endif

static ZONE_DEFINE_TYPE(bank_task_zone, "bank_task",
    struct bank_task, ZC_NONE);
static ZONE_DEFINE_TYPE(bank_account_zone, "bank_account",
    struct bank_account, ZC_NONE);

#define MAX_BANK_TASK     (CONFIG_TASK_MAX)
#define MAX_BANK_ACCOUNT  (CONFIG_TASK_MAX + CONFIG_THREAD_MAX)

#define BANK_ELEMENT_TO_HANDLE(x) (CAST_DOWN(bank_handle_t, (x)))
#define HANDLE_TO_BANK_ELEMENT(x) (CAST_DOWN(bank_element_t, (x)))

/* Need macro since bank_element_t is 4 byte aligned on release kernel and direct type case gives compilation error */
#define CAST_TO_BANK_ELEMENT(x) ((bank_element_t)((void *)(x)))
#define CAST_TO_BANK_TASK(x) ((bank_task_t)((void *)(x)))
#define CAST_TO_BANK_ACCOUNT(x) ((bank_account_t)((void *)(x)))

ipc_voucher_attr_control_t  bank_voucher_attr_control;    /* communication channel from ATM to voucher system */

static ledger_template_t bank_ledger_template = NULL;
struct _bank_ledger_indices bank_ledgers = { .cpu_time = -1, .energy = -1 };

static bank_task_t bank_task_alloc_init(task_t task);
static bank_account_t bank_account_alloc_init(bank_task_t bank_holder, bank_task_t bank_merchant,
    bank_task_t bank_secureoriginator, bank_task_t bank_proximateprocess, struct thread_group* banktg, struct bank_persona *persona);
static bank_task_t get_bank_task_context(task_t task, boolean_t initialize);
static void bank_task_dealloc(bank_task_t bank_task);
static kern_return_t bank_account_dealloc_with_sync(bank_account_t bank_account, mach_voucher_attr_value_reference_t sync);
static void bank_rollup_chit_to_tasks(ledger_t bill, ledger_t bank_holder_ledger, ledger_t bank_merchant_ledger,
    int bank_holder_pid, int bank_merchant_pid);
static ledger_t bank_get_bank_task_ledger_with_ref(bank_task_t bank_task);
static void bank_destroy_bank_task_ledger(bank_task_t bank_task);
static void init_bank_ledgers(void);
static boolean_t bank_task_is_propagate_entitled(task_t t);
static boolean_t bank_task_is_persona_modify_entitled(task_t t);
static struct thread_group *bank_get_bank_task_thread_group(bank_task_t bank_task __unused);
static struct thread_group *bank_get_bank_account_thread_group(bank_account_t bank_account __unused);
static uint64_t bank_get_bank_account_holder_resource_coalition_id(bank_account_t bank_account __unused);
static boolean_t bank_verify_persona_id(uint32_t persona_id, struct bank_persona *persona_out);
static boolean_t bank_task_can_adopt_persona(bank_task_t bank_merchant, struct bank_persona *persona);
static void bank_task_get_persona(bank_task_t bank_task, struct bank_persona *persona_out);

/* lock to protect task->bank_context transition */
static LCK_GRP_DECLARE(bank_lock_grp, "bank_lock");
static LCK_ATTR_DECLARE(bank_lock_attr, 0, 0);
static LCK_SPIN_DECLARE_ATTR(g_bank_task_lock_data, &bank_lock_grp, &bank_lock_attr);

static TUNABLE(bool, disable_persona_propagate_check,
    "disable_persona_propagate_check", false);

#define global_bank_task_lock() \
	lck_spin_lock_grp(&g_bank_task_lock_data, &bank_lock_grp)
#define global_bank_task_lock_try() \
	lck_spin_try_lock_grp(&g_bank_task_lock_data, &bank_lock_grp)
#define global_bank_task_unlock() \
	lck_spin_unlock(&g_bank_task_lock_data)

extern uint64_t proc_uniqueid(void *p);
struct proc;
extern int32_t proc_pid(struct proc *p);
extern int32_t proc_pidversion(void *p);
extern uint32_t proc_getuid(void *p);
extern uint32_t proc_getgid(void *p);
extern void proc_getexecutableuuid(void *p, unsigned char *uuidbuf, unsigned long size);
extern int kauth_cred_issuser(void *cred);
extern void* kauth_cred_get(void);

struct persona_t;
extern struct persona *proc_persona_get(void *p);
extern struct persona *persona_lookup(uint32_t id);
extern void persona_put(struct persona *persona);
extern uint32_t persona_get_id(struct persona *persona);
extern uint32_t persona_get_uid(struct persona *persona);
extern boolean_t persona_is_adoption_allowed(struct persona *persona);

kern_return_t
bank_release_value(
	ipc_voucher_attr_manager_t __assert_only manager,
	mach_voucher_attr_key_t __assert_only key,
	mach_voucher_attr_value_handle_t value,
	mach_voucher_attr_value_reference_t sync);

kern_return_t
bank_get_value(
	ipc_voucher_attr_manager_t __assert_only manager,
	mach_voucher_attr_key_t __assert_only key,
	mach_voucher_attr_recipe_command_t command,
	mach_voucher_attr_value_handle_array_t prev_values,
	mach_msg_type_number_t __assert_only prev_value_count,
	mach_voucher_attr_content_t recipe,
	mach_voucher_attr_content_size_t recipe_size,
	mach_voucher_attr_value_handle_t *out_value,
	mach_voucher_attr_value_flags_t  *out_flags,
	ipc_voucher_t *out_value_voucher);

kern_return_t
bank_extract_content(
	ipc_voucher_attr_manager_t __assert_only manager,
	mach_voucher_attr_key_t __assert_only key,
	mach_voucher_attr_value_handle_array_t values,
	mach_msg_type_number_t value_count,
	mach_voucher_attr_recipe_command_t *out_command,
	mach_voucher_attr_content_t out_recipe,
	mach_voucher_attr_content_size_t *in_out_recipe_size);

kern_return_t
bank_command(
	ipc_voucher_attr_manager_t __assert_only manager,
	mach_voucher_attr_key_t __assert_only key,
	mach_voucher_attr_value_handle_array_t values,
	mach_msg_type_number_t value_count,
	mach_voucher_attr_command_t command,
	mach_voucher_attr_content_t in_content,
	mach_voucher_attr_content_size_t in_content_size,
	mach_voucher_attr_content_t out_content,
	mach_voucher_attr_content_size_t *in_out_content_size);


/*
 * communication channel from voucher system to ATM
 */
const struct ipc_voucher_attr_manager bank_manager = {
	.ivam_release_value    = bank_release_value,
	.ivam_get_value        = bank_get_value,
	.ivam_extract_content  = bank_extract_content,
	.ivam_command          = bank_command,
	.ivam_flags            = (IVAM_FLAGS_SUPPORT_SEND_PREPROCESS | IVAM_FLAGS_SUPPORT_RECEIVE_POSTPROCESS),
};


#if DEVELOPMENT || DEBUG
LCK_GRP_DECLARE(bank_dev_lock_grp, "bank_dev_lock");
LCK_MTX_DECLARE(bank_tasks_list_lock, &bank_dev_lock_grp);
LCK_MTX_DECLARE(bank_accounts_list_lock, &bank_dev_lock_grp);
queue_head_t bank_tasks_list = QUEUE_HEAD_INITIALIZER(bank_tasks_list);
queue_head_t bank_accounts_list = QUEUE_HEAD_INITIALIZER(bank_accounts_list);
#endif

/*
 * Routine: bank_init
 * Purpose: Initialize the BANK subsystem.
 * Returns: None.
 */
__startup_func
static void
bank_init(void)
{
	init_bank_ledgers();

	/* Register the bank manager with the Vouchers sub system. */
	ipc_register_well_known_mach_voucher_attr_manager(
		&bank_manager,
		0,
		MACH_VOUCHER_ATTR_KEY_BANK,
		&bank_voucher_attr_control);

	kprintf("BANK subsystem is initialized\n");
}
STARTUP(MACH_IPC, STARTUP_RANK_FIRST, bank_init);


/*
 * BANK Resource Manager Routines.
 */


/*
 * Routine: bank_release_value
 * Purpose: Release a value, if sync matches the sync count in value.
 * Returns: KERN_SUCCESS: on Successful deletion.
 *          KERN_FAILURE: if sync value does not matches.
 */
kern_return_t
bank_release_value(
	ipc_voucher_attr_manager_t              __assert_only manager,
	mach_voucher_attr_key_t                 __assert_only key,
	mach_voucher_attr_value_handle_t                      value,
	mach_voucher_attr_value_reference_t               sync)
{
	bank_task_t bank_task = BANK_TASK_NULL;
	bank_element_t bank_element = BANK_ELEMENT_NULL;
	bank_account_t bank_account = BANK_ACCOUNT_NULL;
	kern_return_t kr = KERN_SUCCESS;

	assert(MACH_VOUCHER_ATTR_KEY_BANK == key);
	assert(manager == &bank_manager);


	bank_element = HANDLE_TO_BANK_ELEMENT(value);
	/* Voucher system should never release the default or persistent value */
	assert(bank_element != BANK_DEFAULT_VALUE && bank_element != BANK_DEFAULT_TASK_VALUE);

	if (bank_element == BANK_DEFAULT_VALUE || bank_element == BANK_DEFAULT_TASK_VALUE) {
		/* Return success for default and default task value */
		return KERN_SUCCESS;
	}


	if (bank_element->be_type == BANK_TASK) {
		bank_task = CAST_TO_BANK_TASK(bank_element);

		/* Checking of the made ref with sync and clearing of voucher ref should be done under a lock */
		lck_mtx_lock(&bank_task->bt_acc_to_pay_lock);
		if (bank_task->bt_made != sync) {
			lck_mtx_unlock(&bank_task->bt_acc_to_pay_lock);
			return KERN_FAILURE;
		}

		bank_task_made_release_num(bank_task, sync);
		assert(bank_task->bt_voucher_ref == 1);
		bank_task->bt_voucher_ref = 0;
		lck_mtx_unlock(&bank_task->bt_acc_to_pay_lock);

		bank_task_dealloc(bank_task);
	} else if (bank_element->be_type == BANK_ACCOUNT) {
		bank_account = CAST_TO_BANK_ACCOUNT(bank_element);
		kr = bank_account_dealloc_with_sync(bank_account, sync);
	} else {
		panic("Bogus bank type: %d passed in get_value", bank_element->be_type);
	}

	return kr;
}


/*
 * Routine: bank_get_value
 *
 * This function uses the recipe to create a bank attribute for a voucher.
 */
kern_return_t
bank_get_value(
	ipc_voucher_attr_manager_t              __assert_only manager,
	mach_voucher_attr_key_t                 __assert_only key,
	mach_voucher_attr_recipe_command_t                command,
	mach_voucher_attr_value_handle_array_t        prev_values,
	mach_msg_type_number_t                        prev_value_count,
	mach_voucher_attr_content_t                   recipe,
	mach_voucher_attr_content_size_t              recipe_size,
	mach_voucher_attr_value_handle_t             *out_value,
	mach_voucher_attr_value_flags_t              *out_flags,
	ipc_voucher_t                                            *out_value_voucher)
{
	bank_task_t bank_holder = BANK_TASK_NULL;
	bank_task_t bank_merchant = BANK_TASK_NULL;
	bank_task_t bank_secureoriginator = BANK_TASK_NULL;
	bank_task_t bank_proximateprocess = BANK_TASK_NULL;
	bank_element_t bank_element = BANK_ELEMENT_NULL;
	bank_account_t bank_account = BANK_ACCOUNT_NULL;
	bank_account_t old_bank_account = BANK_ACCOUNT_NULL;
	mach_voucher_attr_value_handle_t bank_handle;
	task_t task;
	kern_return_t kr = KERN_SUCCESS;
	mach_msg_type_number_t i;
	struct thread_group *thread_group = NULL;
	struct thread_group *cur_thread_group = NULL;
	struct bank_persona persona = {
		.persona_id = PERSONA_ID_NONE,
		.persona_uid = KAUTH_UID_NONE
	};

	assert(MACH_VOUCHER_ATTR_KEY_BANK == key);
	assert(manager == &bank_manager);

	/* never an out voucher */
	*out_value_voucher = IPC_VOUCHER_NULL;
	*out_flags = MACH_VOUCHER_ATTR_VALUE_FLAGS_NONE;

	switch (command) {
	case MACH_VOUCHER_ATTR_BANK_CREATE:

		/* It returns the default task value. This value is replaced by
		 * an actual bank task reference, by using a recipe with
		 * MACH_VOUCHER_ATTR_SEND_PREPROCESS command.
		 */
		*out_value = BANK_ELEMENT_TO_HANDLE(BANK_DEFAULT_TASK_VALUE);
		*out_flags = MACH_VOUCHER_ATTR_VALUE_FLAGS_PERSIST;
		break;

	case MACH_VOUCHER_ATTR_BANK_MODIFY_PERSONA:

		/* It creates a bank account attribute value with a new persona id
		 * and auto-redeems it on behalf of the bank_holder.
		 */
		*out_value = BANK_ELEMENT_TO_HANDLE(BANK_DEFAULT_VALUE);

		for (i = 0; i < prev_value_count; i++) {
			bank_handle = prev_values[i];
			bank_element = HANDLE_TO_BANK_ELEMENT(bank_handle);

			/* Expect a pre-processed attribute value */
			if (bank_element == BANK_DEFAULT_VALUE || bank_element == BANK_DEFAULT_TASK_VALUE) {
				continue;
			}

			if (!bank_task_is_persona_modify_entitled(current_task())) {
				return KERN_NO_ACCESS;
			}

			struct persona_modify_info pmi = {};
			if (recipe_size == sizeof(pmi)) {
				memcpy((void *)&pmi, recipe, sizeof(pmi));
			} else {
				return KERN_INVALID_ARGUMENT;
			}

			/* Verify if the persona id is valid */
			if (!bank_verify_persona_id(pmi.persona_id, &persona)) {
				return KERN_INVALID_ARGUMENT;
			}

			/* Update the persona id only if the bank element is a bank task.
			 * This ensures that the bank_holder can be trusted.
			 */
			if (bank_element->be_type == BANK_TASK) {
				bank_holder = CAST_TO_BANK_TASK(bank_element);
				/* Ensure that the requestor validated by userspace matches
				 * the bank_holder
				 */
				if (pmi.unique_pid != bank_holder->bt_unique_pid) {
					return KERN_INVALID_CAPABILITY;
				}
				bank_merchant = bank_holder;
				bank_secureoriginator = bank_holder;
				bank_proximateprocess = bank_holder;
				thread_group = bank_get_bank_task_thread_group(bank_holder);
			} else if (bank_element->be_type == BANK_ACCOUNT) {
				return KERN_INVALID_ARGUMENT;
			} else {
				panic("Bogus bank type: %d passed in get_value", bank_element->be_type);
			}

			if (!bank_task_can_adopt_persona(bank_merchant, &persona)) {
				return KERN_INVALID_ARGUMENT;
			}

			if (bank_holder->bt_persona_id == persona.persona_id) {
				lck_mtx_lock(&bank_holder->bt_acc_to_pay_lock);
				bank_task_made_reference(bank_holder);
				if (bank_holder->bt_voucher_ref == 0) {
					/* Take a ref for voucher system, if voucher system does not have a ref */
					bank_task_reference(bank_holder);
					bank_holder->bt_voucher_ref = 1;
				}
				lck_mtx_unlock(&bank_holder->bt_acc_to_pay_lock);

				*out_value = BANK_ELEMENT_TO_HANDLE(bank_holder);
				return kr;
			}

			bank_account = bank_account_alloc_init(bank_holder, bank_merchant,
			    bank_secureoriginator, bank_proximateprocess,
			    thread_group, &persona);
			if (bank_account == BANK_ACCOUNT_NULL) {
				return KERN_RESOURCE_SHORTAGE;
			}

			*out_value = BANK_ELEMENT_TO_HANDLE(bank_account);
			return kr;
		}
		break;

	case MACH_VOUCHER_ATTR_AUTO_REDEEM:

		/* It creates a bank account with the bank_merchant set to the current task.
		 * A bank attribute voucher needs to be redeemed before it can be adopted by
		 * it's threads.
		 */
		for (i = 0; i < prev_value_count; i++) {
			bank_handle = prev_values[i];
			bank_element = HANDLE_TO_BANK_ELEMENT(bank_handle);

			/* Should not have received default task value from an IPC */
			if (bank_element == BANK_DEFAULT_VALUE || bank_element == BANK_DEFAULT_TASK_VALUE) {
				continue;
			}

			task = current_task();
			if (bank_element->be_type == BANK_TASK) {
				bank_holder = CAST_TO_BANK_TASK(bank_element);
				bank_secureoriginator = bank_holder;
				bank_proximateprocess = bank_holder;
				thread_group = bank_get_bank_task_thread_group(bank_holder);
				bank_task_get_persona(bank_holder, &persona);
			} else if (bank_element->be_type == BANK_ACCOUNT) {
				old_bank_account = CAST_TO_BANK_ACCOUNT(bank_element);
				bank_holder = old_bank_account->ba_holder;
				bank_secureoriginator = old_bank_account->ba_secureoriginator;
				bank_proximateprocess = old_bank_account->ba_proximateprocess;
				thread_group = bank_get_bank_account_thread_group(old_bank_account);
				persona = old_bank_account->ba_so_persona;
			} else {
				panic("Bogus bank type: %d passed in get_value", bank_element->be_type);
			}

			bank_merchant = get_bank_task_context(task, FALSE);
			if (bank_merchant == BANK_TASK_NULL) {
				return KERN_RESOURCE_SHORTAGE;
			}

			cur_thread_group = bank_get_bank_task_thread_group(bank_merchant);

			/* Change voucher thread group to current thread group for Apps */
			if (task_is_app(task)) {
				thread_group = cur_thread_group;
			}

			/* If persona adoption is disallowed, reset it to the current task value */
			if (!bank_task_can_adopt_persona(bank_merchant, &persona)) {
				bank_task_get_persona(bank_merchant, &persona);
			}

			/* Check if trying to redeem for self task, return the default bank task */
			if (bank_holder == bank_merchant &&
			    bank_holder == bank_secureoriginator &&
			    bank_holder == bank_proximateprocess &&
			    thread_group == cur_thread_group &&
			    persona.persona_id == bank_holder->bt_persona_id) {
				*out_value = BANK_ELEMENT_TO_HANDLE(BANK_DEFAULT_TASK_VALUE);
				*out_flags = MACH_VOUCHER_ATTR_VALUE_FLAGS_PERSIST;
				return kr;
			}

			bank_account = bank_account_alloc_init(bank_holder, bank_merchant,
			    bank_secureoriginator, bank_proximateprocess,
			    thread_group, &persona);
			if (bank_account == BANK_ACCOUNT_NULL) {
				return KERN_RESOURCE_SHORTAGE;
			}

			*out_value = BANK_ELEMENT_TO_HANDLE(bank_account);
			return kr;
		}

		*out_value = BANK_ELEMENT_TO_HANDLE(BANK_DEFAULT_VALUE);
		break;

	case MACH_VOUCHER_ATTR_SEND_PREPROCESS:

		for (i = 0; i < prev_value_count; i++) {
			bank_handle = prev_values[i];
			bank_element = HANDLE_TO_BANK_ELEMENT(bank_handle);

			if (bank_element == BANK_DEFAULT_VALUE) {
				continue;
			}

			task = current_task();
			if (bank_element == BANK_DEFAULT_TASK_VALUE) {
				bank_element = CAST_TO_BANK_ELEMENT(get_bank_task_context(task, FALSE));
			}

			if (bank_element->be_type == BANK_TASK) {
				bank_holder = CAST_TO_BANK_TASK(bank_element);
				bank_secureoriginator = bank_holder;
				thread_group = bank_get_bank_task_thread_group(bank_holder);
				bank_task_get_persona(bank_holder, &persona);
			} else if (bank_element->be_type == BANK_ACCOUNT) {
				old_bank_account = CAST_TO_BANK_ACCOUNT(bank_element);
				bank_holder = old_bank_account->ba_holder;
				bank_secureoriginator = old_bank_account->ba_secureoriginator;
				thread_group = bank_get_bank_account_thread_group(old_bank_account);
				persona = old_bank_account->ba_so_persona;
			} else {
				panic("Bogus bank type: %d passed in get_value", bank_element->be_type);
			}

			bank_merchant = get_bank_task_context(task, FALSE);
			if (bank_merchant == BANK_TASK_NULL) {
				return KERN_RESOURCE_SHORTAGE;
			}

			cur_thread_group = bank_get_bank_task_thread_group(bank_merchant);

			/*
			 * If the process doesn't have secure persona entitlement,
			 * then replace the secure originator to current task.
			 * Also update the persona to match that of the secure originator.
			 */
			if (bank_merchant->bt_hasentitlement == 0) {
				KERNEL_DEBUG_CONSTANT_IST(KDEBUG_TRACE,
				    (BANK_CODE(BANK_ACCOUNT_INFO, (BANK_SECURE_ORIGINATOR_CHANGED))) | DBG_FUNC_NONE,
				    bank_secureoriginator->bt_pid, bank_merchant->bt_pid, 0, 0, 0);
				bank_secureoriginator = bank_merchant;
				bank_task_get_persona(bank_merchant, &persona);
			}

			bank_proximateprocess = bank_merchant;

			/* Check if trying to pre-process for self task, return the bank task */
			if (bank_holder == bank_merchant &&
			    bank_holder == bank_secureoriginator &&
			    bank_holder == bank_proximateprocess &&
			    thread_group == cur_thread_group &&
			    persona.persona_id == bank_holder->bt_persona_id) {
				lck_mtx_lock(&bank_holder->bt_acc_to_pay_lock);
				bank_task_made_reference(bank_holder);
				if (bank_holder->bt_voucher_ref == 0) {
					/* Take a ref for voucher system, if voucher system does not have a ref */
					bank_task_reference(bank_holder);
					bank_holder->bt_voucher_ref = 1;
				}
				lck_mtx_unlock(&bank_holder->bt_acc_to_pay_lock);

				*out_value = BANK_ELEMENT_TO_HANDLE(bank_holder);
				return kr;
			}
			bank_account = bank_account_alloc_init(bank_holder, bank_merchant,
			    bank_secureoriginator, bank_proximateprocess,
			    thread_group, &persona);
			if (bank_account == BANK_ACCOUNT_NULL) {
				return KERN_RESOURCE_SHORTAGE;
			}

			*out_value = BANK_ELEMENT_TO_HANDLE(bank_account);
			return kr;
		}

		*out_value = BANK_ELEMENT_TO_HANDLE(BANK_DEFAULT_VALUE);
		break;

	case MACH_VOUCHER_ATTR_REDEEM:
		/* This command expects that the bank attribute has been auto-redeemed
		 * and returns a reference to that bank account value.
		 */
		for (i = 0; i < prev_value_count; i++) {
			bank_handle = prev_values[i];
			bank_element = HANDLE_TO_BANK_ELEMENT(bank_handle);

			if (bank_element == BANK_DEFAULT_VALUE) {
				continue;
			}

			if (bank_element == BANK_DEFAULT_TASK_VALUE) {
				*out_value = BANK_ELEMENT_TO_HANDLE(BANK_DEFAULT_TASK_VALUE);
				*out_flags = MACH_VOUCHER_ATTR_VALUE_FLAGS_PERSIST;
				return kr;
			}

			task = current_task();
			if (bank_element->be_type == BANK_TASK) {
				bank_holder =  CAST_TO_BANK_TASK(bank_element);
				if (bank_holder == get_bank_task_context(task, FALSE)) {
					*out_value = BANK_ELEMENT_TO_HANDLE(BANK_DEFAULT_TASK_VALUE);
					*out_flags = MACH_VOUCHER_ATTR_VALUE_FLAGS_PERSIST;
				} else {
					kr = KERN_INVALID_CAPABILITY;
				}
				return kr;
			} else if (bank_element->be_type == BANK_ACCOUNT) {
				bank_account = CAST_TO_BANK_ACCOUNT(bank_element);
				bank_merchant = bank_account->ba_merchant;
				if (bank_merchant != get_bank_task_context(task, FALSE)) {
					/* This error can be used to verify if the task can
					 * adopt the voucher.
					 */
					kr = KERN_INVALID_CAPABILITY;
					return kr;
				}
				bank_account_made_reference(bank_account);
				*out_value = BANK_ELEMENT_TO_HANDLE(bank_account);
				return kr;
			} else {
				panic("Bogus bank type: %d passed in get_value", bank_element->be_type);
			}
		}

		*out_value = BANK_ELEMENT_TO_HANDLE(BANK_DEFAULT_VALUE);
		break;

	default:
		kr = KERN_INVALID_ARGUMENT;
		break;
	}

	return kr;
}


/*
 * Routine: bank_extract_content
 * Purpose: Extract a set of aid from an array of voucher values.
 * Returns: KERN_SUCCESS: on Success.
 *          KERN_FAILURE: one of the value is not present in the hash.
 *          KERN_NO_SPACE: insufficeint buffer provided to fill an array of aid.
 */
kern_return_t
bank_extract_content(
	ipc_voucher_attr_manager_t      __assert_only manager,
	mach_voucher_attr_key_t         __assert_only key,
	mach_voucher_attr_value_handle_array_t        values,
	mach_msg_type_number_t                                value_count,
	mach_voucher_attr_recipe_command_t           *out_command,
	mach_voucher_attr_content_t                   out_recipe,
	mach_voucher_attr_content_size_t             *in_out_recipe_size)
{
	bank_task_t bank_task = BANK_TASK_NULL;
	bank_element_t bank_element = BANK_ELEMENT_NULL;
	bank_account_t bank_account = BANK_ACCOUNT_NULL;
	mach_voucher_attr_value_handle_t bank_handle;
	char buf[MACH_VOUCHER_BANK_CONTENT_SIZE];
	mach_msg_type_number_t i;

	assert(MACH_VOUCHER_ATTR_KEY_BANK == key);
	assert(manager == &bank_manager);

	for (i = 0; i < value_count && *in_out_recipe_size > 0; i++) {
		bank_handle = values[i];
		bank_element = HANDLE_TO_BANK_ELEMENT(bank_handle);
		if (bank_element == BANK_DEFAULT_VALUE) {
			continue;
		}

		if (bank_element == BANK_DEFAULT_TASK_VALUE) {
			bank_element = CAST_TO_BANK_ELEMENT(get_bank_task_context(current_task(), FALSE));
		}

		if (MACH_VOUCHER_BANK_CONTENT_SIZE > *in_out_recipe_size) {
			*in_out_recipe_size = 0;
			return KERN_NO_SPACE;
		}

		if (bank_element->be_type == BANK_TASK) {
			bank_task = CAST_TO_BANK_TASK(bank_element);
			snprintf(buf, MACH_VOUCHER_BANK_CONTENT_SIZE,
			    " Bank Context for a pid %d\n", bank_task->bt_pid);
		} else if (bank_element->be_type == BANK_ACCOUNT) {
			bank_account = CAST_TO_BANK_ACCOUNT(bank_element);
			snprintf(buf, MACH_VOUCHER_BANK_CONTENT_SIZE,
			    " Bank Account linking holder pid %d with merchant pid %d, originator PID/persona: %d, %u and proximate PID/persona: %d, %u\n",
			    bank_account->ba_holder->bt_pid,
			    bank_account->ba_merchant->bt_pid,
			    bank_account->ba_secureoriginator->bt_pid,
			    bank_account->ba_so_persona.persona_id,
			    bank_account->ba_proximateprocess->bt_pid,
			    bank_account->ba_proximateprocess->bt_persona_id);
		} else {
			panic("Bogus bank type: %d passed in get_value", bank_element->be_type);
		}

		memcpy(&out_recipe[0], buf, strlen(buf) + 1);
		*out_command = MACH_VOUCHER_ATTR_BANK_NULL;
		*in_out_recipe_size = (mach_voucher_attr_content_size_t)strlen(buf) + 1;
		return KERN_SUCCESS;
	}

	return KERN_SUCCESS;
}

/*
 * Routine: bank_command
 * Purpose: Execute a command against a set of bank values.
 * Returns: KERN_SUCCESS: On successful execution of command.
 *           KERN_FAILURE: On failure.
 */
kern_return_t
bank_command(
	ipc_voucher_attr_manager_t                 __assert_only manager,
	mach_voucher_attr_key_t                    __assert_only key,
	mach_voucher_attr_value_handle_array_t  __unused values,
	mach_msg_type_number_t                  __unused value_count,
	mach_voucher_attr_command_t              __unused command,
	mach_voucher_attr_content_t        __unused in_content,
	mach_voucher_attr_content_size_t   __unused in_content_size,
	mach_voucher_attr_content_t        __unused out_content,
	mach_voucher_attr_content_size_t   __unused *out_content_size)
{
	bank_task_t bank_task = BANK_TASK_NULL;
	bank_task_t bank_merchant = BANK_TASK_NULL;
	bank_task_t bank_secureoriginator = BANK_TASK_NULL;
	bank_task_t bank_proximateprocess = BANK_TASK_NULL;
	struct persona_token *token = NULL;
	bank_element_t bank_element = BANK_ELEMENT_NULL;
	bank_account_t bank_account = BANK_ACCOUNT_NULL;
	mach_voucher_attr_value_handle_t bank_handle;
	mach_msg_type_number_t i;
	task_t task;
	int32_t pid;
	uint32_t persona_id;
	boolean_t adopt_any_persona = FALSE;

	assert(MACH_VOUCHER_ATTR_KEY_BANK == key);
	assert(manager == &bank_manager);

	switch (command) {
	case BANK_ORIGINATOR_PID:

		if ((sizeof(pid)) > *out_content_size) {
			*out_content_size = 0;
			return KERN_NO_SPACE;
		}

		for (i = 0; i < value_count; i++) {
			bank_handle = values[i];
			bank_element = HANDLE_TO_BANK_ELEMENT(bank_handle);
			if (bank_element == BANK_DEFAULT_VALUE) {
				continue;
			}

			if (bank_element == BANK_DEFAULT_TASK_VALUE) {
				bank_element = CAST_TO_BANK_ELEMENT(get_bank_task_context(current_task(), FALSE));
			}

			if (bank_element->be_type == BANK_TASK) {
				bank_task = CAST_TO_BANK_TASK(bank_element);
			} else if (bank_element->be_type == BANK_ACCOUNT) {
				bank_account = CAST_TO_BANK_ACCOUNT(bank_element);
				bank_task = bank_account->ba_holder;
			} else {
				panic("Bogus bank type: %d passed in voucher_command", bank_element->be_type);
			}
			pid = bank_task->bt_pid;

			memcpy(&out_content[0], &pid, sizeof(pid));
			*out_content_size = (mach_voucher_attr_content_size_t)sizeof(pid);
			return KERN_SUCCESS;
		}
		/* In the case of no value, return error KERN_INVALID_VALUE */
		*out_content_size = 0;
		return KERN_INVALID_VALUE;

	case BANK_PERSONA_TOKEN:

		if ((sizeof(struct persona_token)) > *out_content_size) {
			*out_content_size = 0;
			return KERN_NO_SPACE;
		}
		for (i = 0; i < value_count; i++) {
			bank_handle = values[i];
			bank_element = HANDLE_TO_BANK_ELEMENT(bank_handle);
			if (bank_element == BANK_DEFAULT_VALUE) {
				continue;
			}

			if (bank_element == BANK_DEFAULT_TASK_VALUE) {
				bank_element = CAST_TO_BANK_ELEMENT(get_bank_task_context(current_task(), FALSE));
			}

			if (bank_element->be_type == BANK_TASK) {
				*out_content_size = 0;
				return KERN_INVALID_OBJECT;
			} else if (bank_element->be_type == BANK_ACCOUNT) {
				bank_account = CAST_TO_BANK_ACCOUNT(bank_element);
				bank_secureoriginator = bank_account->ba_secureoriginator;
				bank_proximateprocess = bank_account->ba_proximateprocess;
			} else {
				panic("Bogus bank type: %d passed in voucher_command", bank_element->be_type);
			}
			token = (struct persona_token *)(void *)&out_content[0];
			memcpy(&token->originator, &bank_secureoriginator->bt_proc_persona, sizeof(struct proc_persona_info));
			memcpy(&token->proximate, &bank_proximateprocess->bt_proc_persona, sizeof(struct proc_persona_info));

			*out_content_size = (mach_voucher_attr_content_size_t)sizeof(*token);
			return KERN_SUCCESS;
		}
		/* In the case of no value, return error KERN_INVALID_VALUE */
		*out_content_size = 0;
		return KERN_INVALID_VALUE;

	case BANK_PERSONA_ID:

		if ((sizeof(persona_id)) > *out_content_size) {
			*out_content_size = 0;
			return KERN_NO_SPACE;
		}

		for (i = 0; i < value_count; i++) {
			bank_handle = values[i];
			bank_element = HANDLE_TO_BANK_ELEMENT(bank_handle);
			if (bank_element == BANK_DEFAULT_VALUE) {
				continue;
			}

			if (bank_element == BANK_DEFAULT_TASK_VALUE) {
				bank_element = CAST_TO_BANK_ELEMENT(get_bank_task_context(current_task(), FALSE));
			}

			if (bank_element->be_type == BANK_TASK) {
				bank_task = CAST_TO_BANK_TASK(bank_element);
				persona_id = bank_task->bt_persona_id;
			} else if (bank_element->be_type == BANK_ACCOUNT) {
				bank_account = CAST_TO_BANK_ACCOUNT(bank_element);
				persona_id = bank_account->ba_so_persona.persona_id;
			} else {
				panic("Bogus bank type: %d passed in voucher_command", bank_element->be_type);
			}

			memcpy(out_content, &persona_id, sizeof(persona_id));
			*out_content_size = (mach_voucher_attr_content_size_t)sizeof(persona_id);
			return KERN_SUCCESS;
		}
		/* In the case of no value, return error KERN_INVALID_VALUE */
		*out_content_size = 0;
		return KERN_INVALID_VALUE;

	case BANK_PERSONA_ADOPT_ANY:
		if ((sizeof(boolean_t)) > *out_content_size) {
			*out_content_size = 0;
			return KERN_NO_SPACE;
		}

		task = current_task();
		bank_merchant = get_bank_task_context(task, FALSE);
		if (bank_merchant == BANK_TASK_NULL) {
			*out_content_size = 0;
			return KERN_RESOURCE_SHORTAGE;
		}

		adopt_any_persona = (bank_merchant->bt_flags & PROC_PERSONA_INFO_FLAG_ADOPTION_ALLOWED) != 0;
		memcpy(out_content, &adopt_any_persona, sizeof(adopt_any_persona));
		*out_content_size = (mach_voucher_attr_content_size_t)sizeof(adopt_any_persona);
		return KERN_SUCCESS;

	case BANK_ORIGINATOR_PROXIMATE_PID:;
		int32_t *pids_out = (int32_t *)(uintptr_t)&out_content[0];
		if ((sizeof(pid) * 2) > *out_content_size) {
			*out_content_size = 0;
			return KERN_NO_SPACE;
		}

		for (i = 0; i < value_count; i++) {
			bank_task_t bank_origin_task = BANK_TASK_NULL;
			bank_task_t bank_proximate_task = BANK_TASK_NULL;
			bank_handle = values[i];
			bank_element = HANDLE_TO_BANK_ELEMENT(bank_handle);
			if (bank_element == BANK_DEFAULT_VALUE) {
				continue;
			}

			if (bank_element == BANK_DEFAULT_TASK_VALUE) {
				bank_element = CAST_TO_BANK_ELEMENT(get_bank_task_context(current_task(), FALSE));
			}

			if (bank_element->be_type == BANK_TASK) {
				bank_origin_task = CAST_TO_BANK_TASK(bank_element);
			} else if (bank_element->be_type == BANK_ACCOUNT) {
				bank_account = CAST_TO_BANK_ACCOUNT(bank_element);
				bank_origin_task = bank_account->ba_holder;
				bank_proximate_task = bank_account->ba_proximateprocess;
			} else {
				panic("Bogus bank type: %d passed in voucher_command", bank_element->be_type);
			}
			int32_t origin_pid = bank_origin_task->bt_pid;
			int32_t proximate_pid = bank_proximate_task != BANK_TASK_NULL ? bank_proximate_task->bt_pid : -1;

			memcpy(&pids_out[0], &origin_pid, sizeof(origin_pid));
			memcpy(&pids_out[1], &proximate_pid, sizeof(proximate_pid));
			*out_content_size = (mach_voucher_attr_content_size_t)sizeof(pid) * 2;
			return KERN_SUCCESS;
		}
		/* In the case of no value, return error KERN_INVALID_VALUE */
		*out_content_size = 0;
		return KERN_INVALID_VALUE;

	default:
		return KERN_INVALID_ARGUMENT;
	}
	return KERN_SUCCESS;
}

/*
 * Bank Internal Routines.
 */

static boolean_t
bank_task_is_persona_adoption_allowed(task_t task __unused, struct persona *persona)
{
#if defined(XNU_TARGET_OS_OSX)
	/*
	 * On macOS platform binaries spawned in no persona are allowed to adopt
	 * personas.
	 */
	if (persona_get_id(persona) == PERSONA_ID_NONE) {
		return task_get_platform_binary(task) ||
		       IOTaskHasEntitlement(task, ENTITLEMENT_PERSONA_ADOPT_ANY);
	}
#endif
	return persona_is_adoption_allowed(persona);
}

/*
 * Routine: bank_task_alloc_init
 * Purpose: Allocate and initialize a bank task structure.
 * Returns: bank_task_t on Success.
 *          BANK_TASK_NULL: on Failure.
 * Notes:   Leaves the task and ledger blank and has only 1 ref,
 *           needs to take 1 extra ref after the task field is initialized.
 */
static bank_task_t
bank_task_alloc_init(task_t task)
{
	bank_task_t new_bank_task;
	struct persona *persona = NULL;
	void *bsd_info = get_bsdtask_info(task);

	new_bank_task = zalloc_flags(bank_task_zone, Z_WAITOK | Z_NOFAIL);

	new_bank_task->bt_type = BANK_TASK;
	new_bank_task->bt_voucher_ref = 0;
	new_bank_task->bt_made = 0;
	bank_task_ref_init(new_bank_task);

	new_bank_task->bt_ledger = LEDGER_NULL;
	new_bank_task->bt_hasentitlement = !!bank_task_is_propagate_entitled(task);
	queue_init(&new_bank_task->bt_accounts_to_pay);
	queue_init(&new_bank_task->bt_accounts_to_charge);
	lck_mtx_init(&new_bank_task->bt_acc_to_pay_lock, &bank_lock_grp, &bank_lock_attr);
	lck_mtx_init(&new_bank_task->bt_acc_to_charge_lock, &bank_lock_grp, &bank_lock_attr);

	/*
	 * Initialize the persona_id struct
	 */
	persona = proc_persona_get(bsd_info);

	bzero(&new_bank_task->bt_proc_persona, sizeof(new_bank_task->bt_proc_persona));
	new_bank_task->bt_flags = 0;
	if (bank_task_is_persona_adoption_allowed(task, persona)) {
		new_bank_task->bt_flags |= PROC_PERSONA_INFO_FLAG_ADOPTION_ALLOWED;
	}
	new_bank_task->bt_unique_pid = proc_uniqueid(bsd_info);
	new_bank_task->bt_pid = proc_pid(bsd_info);
	new_bank_task->bt_pidversion = proc_pidversion(bsd_info);
	new_bank_task->bt_persona_id = persona_get_id(persona);
	new_bank_task->bt_persona_uid = persona_get_uid(persona);
	new_bank_task->bt_uid = proc_getuid(bsd_info);
	new_bank_task->bt_gid = proc_getgid(bsd_info);
#if CONFIG_THREAD_GROUPS
	new_bank_task->bt_thread_group = thread_group_retain(task_coalition_get_thread_group(task));
#endif /* CONFIG_THREAD_GROUPS */
#if CONFIG_COALITIONS
	coalition_t rsrc_coal = task->coalition[COALITION_TYPE_RESOURCE];
	new_bank_task->bt_rsrc_coal_id = rsrc_coal != COALITION_NULL ? coalition_id(rsrc_coal) : 0;
#endif /* CONFIG_COALITIONS */
	proc_getexecutableuuid(bsd_info, new_bank_task->bt_macho_uuid, sizeof(new_bank_task->bt_macho_uuid));

	persona_put(persona);
	persona = NULL;

#if DEVELOPMENT || DEBUG
	new_bank_task->bt_task = NULL;
	lck_mtx_lock(&bank_tasks_list_lock);
	queue_enter(&bank_tasks_list, new_bank_task, bank_task_t, bt_global_elt);
	lck_mtx_unlock(&bank_tasks_list_lock);
#endif
	return new_bank_task;
}

/*
 * Routine: proc_is_propagate_entitled
 * Purpose: Check if the process is allowed to propagate secure originator.
 * Returns: TRUE if entitled.
 *          FALSE if not.
 */
static boolean_t
bank_task_is_propagate_entitled(task_t t)
{
	/* Check if it has an entitlement which disallows secure originator propagation */
	boolean_t entitled = FALSE;
	entitled = IOTaskHasEntitlement(t, ENTITLEMENT_PERSONA_NO_PROPAGATE);
	if (entitled) {
		return FALSE;
	}

	/* If it's a platform binary, allow propagation by default */
	if (disable_persona_propagate_check || task_get_platform_binary(t)) {
		return TRUE;
	}

	return FALSE;
}

/*
 * Routine: proc_is_persona_modify_entitled
 * Purpose: Check if the process has persona modify entitlement.
 * Returns: TRUE if entitled.
 *          FALSE if not.
 */
static boolean_t
bank_task_is_persona_modify_entitled(task_t t)
{
	boolean_t entitled = FALSE;
	entitled = IOTaskHasEntitlement(t, ENTITLEMENT_PERSONA_MODIFY);
	return entitled;
}

/*
 * Routine: bank_account_alloc_init
 * Purpose: Allocate and Initialize the bank account struct.
 * Returns: bank_account_t : On Success.
 *          BANK_ACCOUNT_NULL: On Failure.
 */
static bank_account_t
bank_account_alloc_init(
	bank_task_t bank_holder,
	bank_task_t bank_merchant,
	bank_task_t bank_secureoriginator,
	bank_task_t bank_proximateprocess,
	struct thread_group *thread_group,
	struct bank_persona *persona)
{
	bank_account_t new_bank_account;
	bank_account_t bank_account;
	boolean_t entry_found = FALSE;
	ledger_t new_ledger = ledger_instantiate(bank_ledger_template, LEDGER_CREATE_INACTIVE_ENTRIES);

	if (new_ledger == LEDGER_NULL) {
		return BANK_ACCOUNT_NULL;
	}

	ledger_entry_setactive(new_ledger, bank_ledgers.cpu_time);
	ledger_entry_setactive(new_ledger, bank_ledgers.energy);
	new_bank_account = zalloc_flags(bank_account_zone, Z_WAITOK | Z_NOFAIL);

	new_bank_account->ba_type = BANK_ACCOUNT;
	new_bank_account->ba_voucher_ref = 0;
	new_bank_account->ba_made = 1;
	bank_account_ref_init(new_bank_account);

	new_bank_account->ba_bill = new_ledger;
	new_bank_account->ba_merchant = bank_merchant;
	new_bank_account->ba_holder = bank_holder;
	new_bank_account->ba_secureoriginator = bank_secureoriginator;
	new_bank_account->ba_proximateprocess = bank_proximateprocess;
#if CONFIG_THREAD_GROUPS
	new_bank_account->ba_thread_group = thread_group;
#endif
	new_bank_account->ba_so_persona = *persona;

	/* Iterate through accounts need to pay list to find the existing entry */
	lck_mtx_lock(&bank_holder->bt_acc_to_pay_lock);
	queue_iterate(&bank_holder->bt_accounts_to_pay, bank_account, bank_account_t, ba_next_acc_to_pay) {
		if (bank_account->ba_merchant != bank_merchant ||
		    bank_account->ba_secureoriginator != bank_secureoriginator ||
		    bank_account->ba_proximateprocess != bank_proximateprocess ||
		    bank_get_bank_account_thread_group(bank_account) != thread_group ||
		    bank_account->ba_so_persona.persona_id != persona->persona_id) {
			continue;
		}

		entry_found = TRUE;
		/* Take a made ref, since this value would be returned to voucher system. */
		bank_account_made_reference(bank_account);
		break;
	}

	if (!entry_found) {
		/*  Create a linkage between the holder and the merchant task, Grab both the list locks before adding it to the list. */
		lck_mtx_lock(&bank_merchant->bt_acc_to_charge_lock);

		/* Add the account entry into Accounts need to pay account link list. */
		queue_enter(&bank_holder->bt_accounts_to_pay, new_bank_account, bank_account_t, ba_next_acc_to_pay);

		/* Add the account entry into Accounts need to charge account link list. */
		queue_enter(&bank_merchant->bt_accounts_to_charge, new_bank_account, bank_account_t, ba_next_acc_to_charge);

		lck_mtx_unlock(&bank_merchant->bt_acc_to_charge_lock);
	}

	lck_mtx_unlock(&bank_holder->bt_acc_to_pay_lock);

	if (entry_found) {
		ledger_dereference(new_ledger);
		zfree(bank_account_zone, new_bank_account);
		return bank_account;
	}

	bank_task_reference(bank_holder);
	bank_task_reference(bank_merchant);
	bank_task_reference(bank_secureoriginator);
	bank_task_reference(bank_proximateprocess);
#if CONFIG_THREAD_GROUPS
	assert(new_bank_account->ba_thread_group != NULL);
	thread_group_retain(new_bank_account->ba_thread_group);
#endif

#if DEVELOPMENT || DEBUG
	new_bank_account->ba_task = NULL;
	lck_mtx_lock(&bank_accounts_list_lock);
	queue_enter(&bank_accounts_list, new_bank_account, bank_account_t, ba_global_elt);
	lck_mtx_unlock(&bank_accounts_list_lock);
#endif

	return new_bank_account;
}

/*
 * Routine: get_bank_task_context
 * Purpose: Get the bank context of the given task
 * Returns: bank_task_t on Success.
 *          BANK_TASK_NULL: on Failure.
 * Note:    Initialize bank context if NULL.
 */
static bank_task_t
get_bank_task_context
(task_t task,
    boolean_t initialize)
{
	bank_task_t bank_task;

	if (task->bank_context || !initialize) {
		assert(task->bank_context != NULL);
		return task->bank_context;
	}

	bank_task = bank_task_alloc_init(task);

	/* Grab the task lock and check if we won the race. */
	task_lock(task);
	if (task->bank_context) {
		task_unlock(task);
		if (bank_task != BANK_TASK_NULL) {
			bank_task_dealloc(bank_task);
		}
		return task->bank_context;
	} else if (bank_task == BANK_TASK_NULL) {
		task_unlock(task);
		return BANK_TASK_NULL;
	}
	/* We won the race. Take a ref on the ledger and initialize bank task. */
	bank_task->bt_ledger = task->ledger;
#if DEVELOPMENT || DEBUG
	bank_task->bt_task = task;
#endif
	ledger_reference(task->ledger);

	/* Grab the global bank task lock before setting the bank context on a task */
	global_bank_task_lock();
	task->bank_context = bank_task;
	global_bank_task_unlock();

	task_unlock(task);

	return bank_task;
}

/*
 * Routine: bank_task_dealloc
 * Purpose: Drops the reference on bank task.
 * Returns: None.
 */
static void
bank_task_dealloc(bank_task_t bank_task)
{
	if (bank_task_release(bank_task) > 0) {
		return;
	}

	assert(queue_empty(&bank_task->bt_accounts_to_pay));
	assert(queue_empty(&bank_task->bt_accounts_to_charge));

	assert(!LEDGER_VALID(bank_task->bt_ledger));
	lck_mtx_destroy(&bank_task->bt_acc_to_pay_lock, &bank_lock_grp);
	lck_mtx_destroy(&bank_task->bt_acc_to_charge_lock, &bank_lock_grp);

#if CONFIG_THREAD_GROUPS
	thread_group_release(bank_task->bt_thread_group);
#endif

#if DEVELOPMENT || DEBUG
	lck_mtx_lock(&bank_tasks_list_lock);
	queue_remove(&bank_tasks_list, bank_task, bank_task_t, bt_global_elt);
	lck_mtx_unlock(&bank_tasks_list_lock);
#endif

	zfree(bank_task_zone, bank_task);
}

/*
 * Routine: bank_account_dealloc_with_sync
 * Purpose: Drop the reference on bank account if the sync matches.
 * Returns: KERN_SUCCESS if sync matches.
 *          KERN_FAILURE on mismatch.
 */
static kern_return_t
bank_account_dealloc_with_sync(
	bank_account_t bank_account,
	mach_voucher_attr_value_reference_t sync)
{
	bank_task_t bank_holder = bank_account->ba_holder;
	bank_task_t bank_merchant = bank_account->ba_merchant;
	bank_task_t bank_secureoriginator = bank_account->ba_secureoriginator;
	bank_task_t bank_proximateprocess = bank_account->ba_proximateprocess;
	ledger_t bank_merchant_ledger = LEDGER_NULL;

	/*
	 * Grab a reference on the bank_merchant_ledger, since we would not be able
	 * to take bt_acc_to_pay_lock for bank_merchant later.
	 */
	bank_merchant_ledger = bank_get_bank_task_ledger_with_ref(bank_merchant);

	/* Grab the acc to pay list lock and check the sync value */
	lck_mtx_lock(&bank_holder->bt_acc_to_pay_lock);

	if (bank_account->ba_made != sync) {
		lck_mtx_unlock(&bank_holder->bt_acc_to_pay_lock);
		if (bank_merchant_ledger) {
			ledger_dereference(bank_merchant_ledger);
		}
		return KERN_FAILURE;
	}

	bank_account_made_release_num(bank_account, sync);

	if (bank_account_release(bank_account) > 0) {
		panic("Releasing a non zero ref bank account %p", bank_account);
	}


	/* Grab both the acc to pay and acc to charge locks */
	lck_mtx_lock(&bank_merchant->bt_acc_to_charge_lock);

	/* No need to take ledger reference for bank_holder ledger since bt_acc_to_pay_lock is locked */
	bank_rollup_chit_to_tasks(bank_account->ba_bill, bank_holder->bt_ledger, bank_merchant_ledger,
	    bank_holder->bt_pid, bank_merchant->bt_pid);

	/* Remove the account entry from Accounts need to pay account link list. */
	queue_remove(&bank_holder->bt_accounts_to_pay, bank_account, bank_account_t, ba_next_acc_to_pay);

	/* Remove the account entry from Accounts need to charge account link list. */
	queue_remove(&bank_merchant->bt_accounts_to_charge, bank_account, bank_account_t, ba_next_acc_to_charge);

	lck_mtx_unlock(&bank_merchant->bt_acc_to_charge_lock);
	lck_mtx_unlock(&bank_holder->bt_acc_to_pay_lock);

	if (bank_merchant_ledger) {
		ledger_dereference(bank_merchant_ledger);
	}
	ledger_dereference(bank_account->ba_bill);

	/* Drop the reference of bank holder and merchant */
	bank_task_dealloc(bank_holder);
	bank_task_dealloc(bank_merchant);
	bank_task_dealloc(bank_secureoriginator);
	bank_task_dealloc(bank_proximateprocess);
#if CONFIG_THREAD_GROUPS
	assert(bank_account->ba_thread_group != NULL);
	thread_group_release(bank_account->ba_thread_group);
#endif

#if DEVELOPMENT || DEBUG
	lck_mtx_lock(&bank_accounts_list_lock);
	queue_remove(&bank_accounts_list, bank_account, bank_account_t, ba_global_elt);
	lck_mtx_unlock(&bank_accounts_list_lock);
#endif

	zfree(bank_account_zone, bank_account);
	return KERN_SUCCESS;
}

/*
 * Routine: bank_rollup_chit_to_tasks
 * Purpose: Debit and Credit holder's and merchant's ledgers.
 * Returns: None.
 */
static void
bank_rollup_chit_to_tasks(
	ledger_t bill,
	ledger_t bank_holder_ledger,
	ledger_t bank_merchant_ledger,
	int bank_holder_pid,
	int bank_merchant_pid)
{
	ledger_amount_t credit;
	ledger_amount_t debit;
	kern_return_t ret;

	if (bank_holder_ledger == bank_merchant_ledger) {
		return;
	}

	ret = ledger_get_entries(bill, bank_ledgers.cpu_time, &credit, &debit);
	if (ret == KERN_SUCCESS) {
		KERNEL_DEBUG_CONSTANT_IST(KDEBUG_TRACE,
		    (BANK_CODE(BANK_ACCOUNT_INFO, (BANK_SETTLE_CPU_TIME))) | DBG_FUNC_NONE,
		    bank_merchant_pid, bank_holder_pid, credit, debit, 0);

		if (bank_holder_ledger) {
			ledger_credit(bank_holder_ledger, task_ledgers.cpu_time_billed_to_me, credit);
			ledger_debit(bank_holder_ledger, task_ledgers.cpu_time_billed_to_me, debit);
		}

		if (bank_merchant_ledger) {
			ledger_credit(bank_merchant_ledger, task_ledgers.cpu_time_billed_to_others, credit);
			ledger_debit(bank_merchant_ledger, task_ledgers.cpu_time_billed_to_others, debit);
		}
	}

	ret = ledger_get_entries(bill, bank_ledgers.energy, &credit, &debit);
	if (ret == KERN_SUCCESS) {
		KERNEL_DEBUG_CONSTANT_IST(KDEBUG_TRACE,
		    (BANK_CODE(BANK_ACCOUNT_INFO, (BANK_SETTLE_ENERGY))) | DBG_FUNC_NONE,
		    bank_merchant_pid, bank_holder_pid, credit, debit, 0);

		if (bank_holder_ledger) {
			ledger_credit(bank_holder_ledger, task_ledgers.energy_billed_to_me, credit);
			ledger_debit(bank_holder_ledger, task_ledgers.energy_billed_to_me, debit);
		}

		if (bank_merchant_ledger) {
			ledger_credit(bank_merchant_ledger, task_ledgers.energy_billed_to_others, credit);
			ledger_debit(bank_merchant_ledger, task_ledgers.energy_billed_to_others, debit);
		}
	}
}



/*
 * Routine: bank_task_destroy
 * Purpose: Drops reference on bank task.
 * Returns: None.
 */
void
bank_task_destroy(task_t task)
{
	bank_task_t bank_task;

	/* Grab the global bank task lock before dropping the ref on task bank context */
	global_bank_task_lock();
	bank_task = task->bank_context;
	task->bank_context = NULL;
	global_bank_task_unlock();

	bank_destroy_bank_task_ledger(bank_task);
	bank_task_dealloc(bank_task);
}

/*
 * Routine: bank_task_initialize
 * Purpose: Initialize the bank context of a task.
 * Returns: None.
 */
void
bank_task_initialize(task_t task)
{
	get_bank_task_context(task, TRUE);
}

/*
 * Routine: init_bank_ledgers
 * Purpose: Initialize template for bank ledgers.
 * Returns: None.
 */
static void
init_bank_ledgers(void)
{
	ledger_template_t t;
	int idx;

	assert(bank_ledger_template == NULL);

	if ((t = ledger_template_create("Bank ledger")) == NULL) {
		panic("couldn't create bank ledger template");
	}

	if ((idx = ledger_entry_add(t, "cpu_time", "sched", "ns")) < 0) {
		panic("couldn't create cpu_time entry for bank ledger template");
	}
	bank_ledgers.cpu_time = idx;

	if ((idx = ledger_entry_add(t, "energy", "power", "nj")) < 0) {
		panic("couldn't create energy entry for bank ledger template");
	}
	bank_ledgers.energy = idx;

	ledger_template_complete(t);
	bank_ledger_template = t;
}

/* Routine: bank_billed_balance_safe
 * Purpose: Walk through all the bank accounts billed to me by other tasks and get the current billing balance.
 *          Called from another task. It takes global bank task lock to make sure the bank context is
 *           not deallocated while accesing it.
 * Returns: cpu balance and energy balance in out paremeters.
 */
void
bank_billed_balance_safe(task_t task, uint64_t *cpu_time, uint64_t *energy)
{
	bank_task_t bank_task = BANK_TASK_NULL;
	ledger_amount_t credit, debit;
	uint64_t cpu_balance = 0;
	uint64_t energy_balance = 0;
	kern_return_t kr;

	/* Task might be in exec, grab the global bank task lock before accessing bank context. */
	global_bank_task_lock();
	/* Grab a reference on bank context */
	if (task->bank_context != NULL) {
		bank_task = task->bank_context;
		bank_task_reference(bank_task);
	}
	global_bank_task_unlock();

	if (bank_task) {
		bank_billed_balance(bank_task, &cpu_balance, &energy_balance);
		bank_task_dealloc(bank_task);
	} else {
		kr = ledger_get_entries(task->ledger, task_ledgers.cpu_time_billed_to_me,
		    &credit, &debit);
		if (kr == KERN_SUCCESS) {
			cpu_balance = credit - debit;
		}
		kr = ledger_get_entries(task->ledger, task_ledgers.energy_billed_to_me,
		    &credit, &debit);
		if (kr == KERN_SUCCESS) {
			energy_balance = credit - debit;
		}
	}

	*cpu_time = cpu_balance;
	*energy = energy_balance;
	return;
}

/*
 * Routine: bank_billed_time
 * Purpose: Walk through the Accounts need to pay account list and get the current billing balance.
 * Returns: cpu balance and energy balance in out paremeters.
 */
void
bank_billed_balance(bank_task_t bank_task, uint64_t *cpu_time, uint64_t *energy)
{
	int64_t cpu_balance = 0;
	int64_t energy_balance = 0;
	bank_account_t bank_account;
	int64_t temp = 0;
	kern_return_t kr;
	if (bank_task == BANK_TASK_NULL) {
		*cpu_time = 0;
		*energy = 0;
		return;
	}

	lck_mtx_lock(&bank_task->bt_acc_to_pay_lock);

	/* bt_acc_to_pay_lock locked, no need to take ledger reference for bt_ledger */
	if (bank_task->bt_ledger != LEDGER_NULL) {
		kr = ledger_get_balance(bank_task->bt_ledger, task_ledgers.cpu_time_billed_to_me, &temp);
		if (kr == KERN_SUCCESS && temp >= 0) {
			cpu_balance += temp;
		}
#if DEVELOPMENT || DEBUG
		else {
			printf("bank_bill_time: ledger_get_balance failed or negative balance in ledger: %lld\n", temp);
		}
#endif /* DEVELOPMENT || DEBUG */

		kr = ledger_get_balance(bank_task->bt_ledger, task_ledgers.energy_billed_to_me, &temp);
		if (kr == KERN_SUCCESS && temp >= 0) {
			energy_balance += temp;
		}
	}

	queue_iterate(&bank_task->bt_accounts_to_pay, bank_account, bank_account_t, ba_next_acc_to_pay) {
		temp = 0;
		kr = ledger_get_balance(bank_account->ba_bill, bank_ledgers.cpu_time, &temp);
		if (kr == KERN_SUCCESS && temp >= 0) {
			cpu_balance += temp;
		}
#if DEVELOPMENT || DEBUG
		else {
			printf("bank_bill_time: ledger_get_balance failed or negative balance in ledger: %lld\n", temp);
		}
#endif /* DEVELOPMENT || DEBUG */

		kr = ledger_get_balance(bank_account->ba_bill, bank_ledgers.energy, &temp);
		if (kr == KERN_SUCCESS && temp >= 0) {
			energy_balance += temp;
		}
	}
	lck_mtx_unlock(&bank_task->bt_acc_to_pay_lock);
	*cpu_time = (uint64_t)cpu_balance;
	*energy = (uint64_t)energy_balance;
	return;
}

/* Routine: bank_serviced_balance_safe
 * Purpose: Walk through the bank accounts billed to other tasks by me and get the current balance to be charged.
 *          Called from another task. It takes global bank task lock to make sure the bank context is
 *           not deallocated while accesing it.
 * Returns: cpu balance and energy balance in out paremeters.
 */
void
bank_serviced_balance_safe(task_t task, uint64_t *cpu_time, uint64_t *energy)
{
	bank_task_t bank_task = BANK_TASK_NULL;
	ledger_amount_t credit, debit;
	uint64_t cpu_balance = 0;
	uint64_t energy_balance = 0;
	kern_return_t kr;

	/* Task might be in exec, grab the global bank task lock before accessing bank context. */
	global_bank_task_lock();
	/* Grab a reference on bank context */
	if (task->bank_context != NULL) {
		bank_task = task->bank_context;
		bank_task_reference(bank_task);
	}
	global_bank_task_unlock();

	if (bank_task) {
		bank_serviced_balance(bank_task, &cpu_balance, &energy_balance);
		bank_task_dealloc(bank_task);
	} else {
		kr = ledger_get_entries(task->ledger, task_ledgers.cpu_time_billed_to_others,
		    &credit, &debit);
		if (kr == KERN_SUCCESS) {
			cpu_balance = credit - debit;
		}

		kr = ledger_get_entries(task->ledger, task_ledgers.energy_billed_to_others,
		    &credit, &debit);
		if (kr == KERN_SUCCESS) {
			energy_balance = credit - debit;
		}
	}

	*cpu_time = cpu_balance;
	*energy = energy_balance;
	return;
}

/*
 * Routine: bank_serviced_balance
 * Purpose: Walk through the Account need to charge account list and get the current balance to be charged.
 * Returns: cpu balance and energy balance in out paremeters.
 */
void
bank_serviced_balance(bank_task_t bank_task, uint64_t *cpu_time, uint64_t *energy)
{
	int64_t cpu_balance = 0;
	int64_t energy_balance = 0;
	bank_account_t bank_account;
	int64_t temp = 0;
	kern_return_t kr;
	ledger_t ledger = LEDGER_NULL;
	if (bank_task == BANK_TASK_NULL) {
		*cpu_time = 0;
		*energy = 0;
		return;
	}

	/* Grab a ledger reference on bt_ledger for bank_task */
	ledger = bank_get_bank_task_ledger_with_ref(bank_task);

	lck_mtx_lock(&bank_task->bt_acc_to_charge_lock);

	if (ledger) {
		kr = ledger_get_balance(ledger, task_ledgers.cpu_time_billed_to_others, &temp);
		if (kr == KERN_SUCCESS && temp >= 0) {
			cpu_balance += temp;
		}
#if DEVELOPMENT || DEBUG
		else {
			printf("bank_serviced_time: ledger_get_balance failed or negative balance in ledger: %lld\n", temp);
		}
#endif /* DEVELOPMENT || DEBUG */

		kr = ledger_get_balance(ledger, task_ledgers.energy_billed_to_others, &temp);
		if (kr == KERN_SUCCESS && temp >= 0) {
			energy_balance += temp;
		}
	}

	queue_iterate(&bank_task->bt_accounts_to_charge, bank_account, bank_account_t, ba_next_acc_to_charge) {
		temp = 0;
		kr = ledger_get_balance(bank_account->ba_bill, bank_ledgers.cpu_time, &temp);
		if (kr == KERN_SUCCESS && temp >= 0) {
			cpu_balance += temp;
		}
#if DEVELOPMENT || DEBUG
		else {
			printf("bank_serviced_time: ledger_get_balance failed or negative balance in ledger: %lld\n", temp);
		}
#endif /* DEVELOPMENT || DEBUG */

		kr = ledger_get_balance(bank_account->ba_bill, bank_ledgers.energy, &temp);
		if (kr == KERN_SUCCESS && temp >= 0) {
			energy_balance += temp;
		}
	}
	lck_mtx_unlock(&bank_task->bt_acc_to_charge_lock);
	if (ledger) {
		ledger_dereference(ledger);
	}
	*cpu_time = (uint64_t)cpu_balance;
	*energy = (uint64_t)energy_balance;
	return;
}

/*
 * Routine: bank_get_voucher_bank_account
 * Purpose: Get the bank account from the voucher.
 * Returns: bank_account if bank_account attribute present in voucher.
 *          NULL on no attribute or no bank_element
 */
static bank_account_t
bank_get_voucher_bank_account(ipc_voucher_t voucher)
{
	bank_element_t bank_element = BANK_ELEMENT_NULL;
	bank_account_t bank_account = BANK_ACCOUNT_NULL;
	mach_voucher_attr_value_handle_t vals[MACH_VOUCHER_ATTR_VALUE_MAX_NESTED];
	mach_voucher_attr_value_handle_array_size_t val_count;
	kern_return_t kr;

	val_count = MACH_VOUCHER_ATTR_VALUE_MAX_NESTED;
	kr = mach_voucher_attr_control_get_values(bank_voucher_attr_control,
	    voucher,
	    vals,
	    &val_count);

	if (kr != KERN_SUCCESS || val_count == 0) {
		return BANK_ACCOUNT_NULL;
	}

	bank_element = HANDLE_TO_BANK_ELEMENT(vals[0]);
	if (bank_element == BANK_DEFAULT_VALUE) {
		return BANK_ACCOUNT_NULL;
	}
	if (bank_element == BANK_DEFAULT_TASK_VALUE) {
		bank_element = CAST_TO_BANK_ELEMENT(get_bank_task_context(current_task(), FALSE));
	}

	if (bank_element->be_type == BANK_TASK) {
		return BANK_ACCOUNT_NULL;
	} else if (bank_element->be_type == BANK_ACCOUNT) {
		bank_account = CAST_TO_BANK_ACCOUNT(bank_element);
		return bank_account;
	} else {
		panic("Bogus bank type: %d passed in bank_get_voucher_bank_account", bank_element->be_type);
	}
	return BANK_ACCOUNT_NULL;
}

/*
 * Routine: bank_get_bank_task_ledger_with_ref
 * Purpose: Get the bank ledger from the bank task and return a reference to it.
 */
static ledger_t
bank_get_bank_task_ledger_with_ref(bank_task_t bank_task)
{
	ledger_t ledger = LEDGER_NULL;

	lck_mtx_lock(&bank_task->bt_acc_to_pay_lock);
	ledger = bank_task->bt_ledger;
	if (ledger) {
		ledger_reference(ledger);
	}
	lck_mtx_unlock(&bank_task->bt_acc_to_pay_lock);

	return ledger;
}

/*
 * Routine: bank_destroy_bank_task_ledger
 * Purpose: Drop the bank task reference on the task ledger.
 */
static void
bank_destroy_bank_task_ledger(bank_task_t bank_task)
{
	ledger_t ledger;

	/* Remove the ledger reference from the bank task */
	lck_mtx_lock(&bank_task->bt_acc_to_pay_lock);
	assert(LEDGER_VALID(bank_task->bt_ledger));
	ledger = bank_task->bt_ledger;
	bank_task->bt_ledger = LEDGER_NULL;
	lck_mtx_unlock(&bank_task->bt_acc_to_pay_lock);

	ledger_dereference(ledger);
}

/*
 * Routine: bank_get_bank_account_ledger
 * Purpose: Get the bankledger from the bank account if ba_merchant different than ba_holder
 */
static ledger_t
bank_get_bank_account_ledger(bank_account_t bank_account)
{
	ledger_t bankledger = LEDGER_NULL;

	if (bank_account != BANK_ACCOUNT_NULL &&
	    bank_account->ba_holder != bank_account->ba_merchant) {
		bankledger = bank_account->ba_bill;
	}

	return bankledger;
}

#if CONFIG_PREADOPT_TG
/*
 * Routine: bank_get_preadopt_thread_group
 * Purpose: Get the thread group from the voucher for preadoption
 *                      (assuming post process is not done on voucher).
 * Returns: thread group for pre adoption
 * Note: Make sure that the receiver is not an App before preadopting the voucher.
 */
kern_return_t
bank_get_preadopt_thread_group(ipc_voucher_t     voucher,
    struct thread_group **banktg)
{
	bank_account_t bank_account = BANK_ACCOUNT_NULL;
	bank_task_t bank_task = BANK_TASK_NULL;
	struct thread_group *thread_group = NULL;
	mach_voucher_attr_value_handle_t vals[MACH_VOUCHER_ATTR_VALUE_MAX_NESTED];
	mach_voucher_attr_value_handle_array_size_t val_count;
	bank_element_t bank_element = BANK_ELEMENT_NULL;

	kern_return_t kr;
	val_count = MACH_VOUCHER_ATTR_VALUE_MAX_NESTED;
	kr = mach_voucher_attr_control_get_values(bank_voucher_attr_control,
	    voucher,
	    vals,
	    &val_count);
	if (kr != KERN_SUCCESS || val_count == 0) {
		goto errorout;
	}
	bank_element = HANDLE_TO_BANK_ELEMENT(vals[0]);
	if (bank_element == BANK_DEFAULT_VALUE) {
		goto errorout;
	}

	if (bank_element == BANK_DEFAULT_TASK_VALUE) {
		bank_element = CAST_TO_BANK_ELEMENT(get_bank_task_context(current_task(), FALSE));
	}

	if (bank_element->be_type == BANK_TASK) {
		bank_task = CAST_TO_BANK_TASK(bank_element);
	} else if (bank_element->be_type == BANK_ACCOUNT) {
		bank_account = CAST_TO_BANK_ACCOUNT(bank_element);
	} else {
		panic("Bogus bank type: %d passed in bank_get_preadopt_thread_group", bank_element->be_type);
	}

errorout:
	if (banktg != NULL) {
		/* If the voucher has bank account, then give the thread group from
		 * bank account. If the voucher has a bank task, this is the sender's bank task,
		 * the receiver will convert the sender's bank task to bank account, so give
		 * thread group from the bank task. */
		if (bank_account != NULL) {
			thread_group = bank_get_bank_account_thread_group(bank_account);
		} else if (bank_task != NULL) {
			thread_group = bank_get_bank_task_thread_group(bank_task);
		}
		*banktg = thread_group;
	}
	return KERN_SUCCESS;
}
#endif

/*
 * Routine: bank_get_bank_task_thread_group
 * Purpose: Get the bank task's thread group from the bank task
 */
static struct thread_group *
bank_get_bank_task_thread_group(bank_task_t bank_task __unused)
{
	struct thread_group *banktg = NULL;

#if CONFIG_THREAD_GROUPS
	if (bank_task != BANK_TASK_NULL) {
		banktg = bank_task->bt_thread_group;
	}
#endif /* CONFIG_THREAD_GROUPS */

	return banktg;
}

/*
 * Routine: bank_get_bank_account_thread_group
 * Purpose: Get the bank account's thread group from the bank account
 */
static struct thread_group *
bank_get_bank_account_thread_group(bank_account_t bank_account __unused)
{
	struct thread_group *banktg = NULL;

#if CONFIG_THREAD_GROUPS
	if (bank_account != BANK_ACCOUNT_NULL) {
		banktg = bank_account->ba_thread_group;
	}
#endif /* CONFIG_THREAD_GROUPS */

	return banktg;
}

static uint64_t
bank_get_bank_account_holder_resource_coalition_id(bank_account_t bank_account __unused)
{
#if CONFIG_COALITIONS
	if (bank_account != BANK_ACCOUNT_NULL) {
		bank_task_t bank_task = bank_account->ba_holder;
		if (bank_task != BANK_TASK_NULL) {
			return bank_task->bt_rsrc_coal_id;
		}
	}
#endif /* CONFIG_COALITIONS */
	return 0;
}

/*
 * Routine: bank_get_bank_ledger_thread_group_and_persona
 * Purpose: Get the bankledger (chit), thread group and persona id from the voucher.
 * Returns: bankledger, thread group if bank_account attribute present in voucher
 *          and persona_id
 */
kern_return_t
bank_get_bank_ledger_thread_group_and_persona(
	ipc_voucher_t     voucher,
	ledger_t          *bankledger,
	struct thread_group **banktg,
	uint32_t *persona_id)
{
	bank_account_t bank_account;
	bank_task_t bank_task;
	struct thread_group *thread_group = NULL;

	bank_account = bank_get_voucher_bank_account(voucher);
	bank_task = get_bank_task_context(current_task(), FALSE);
	if (persona_id != NULL) {
		if (bank_account != BANK_ACCOUNT_NULL) {
			*persona_id = bank_account->ba_so_persona.persona_id;
		} else {
			*persona_id = bank_task->bt_persona_id;
		}
	}
	/*
	 * Use BANK_ACCOUNT_NULL if the ba_holder is same as ba_merchant
	 * and bank account thread group is same as current thread group
	 * i.e. ba_merchant's thread group.
	 *
	 * The bank account might have ba_holder same as ba_merchant but different
	 * thread group if daemon sends a voucher to an App and then App sends the
	 * same voucher back to the daemon (IPC code will replace thread group in the
	 * voucher to App's thread group when it gets auto redeemed by the App).
	 */
	if ((bank_account != NULL) &&
	    (bank_account->ba_holder == bank_account->ba_merchant) &&
	    (bank_get_bank_account_thread_group(bank_account) ==
	    bank_get_bank_task_thread_group(bank_account->ba_merchant))) {
		bank_account = BANK_ACCOUNT_NULL;
	}

	if (bankledger != NULL) {
		*bankledger = bank_get_bank_account_ledger(bank_account);
	}

	if (banktg != NULL) {
		thread_group = bank_get_bank_account_thread_group(bank_account);

		/* Return NULL thread group if voucher has current task's thread group */
		if (thread_group == bank_get_bank_task_thread_group(bank_task)) {
			thread_group = NULL;
		}
		*banktg = thread_group;
	}
	return KERN_SUCCESS;
}

uint64_t
bank_get_bank_ledger_resource_coalition_id(
	ipc_voucher_t     voucher)
{
	bank_account_t bank_account = bank_get_voucher_bank_account(voucher);
	return bank_account != NULL ?
	       bank_get_bank_account_holder_resource_coalition_id(bank_account) :
	       0;
}

/*
 * Routine: bank_swap_thread_bank_ledger
 * Purpose: swap the bank ledger on the thread.
 * Returns: None.
 * Note: Should be only called for current thread or thread which is not started.
 */
void
bank_swap_thread_bank_ledger(thread_t thread __unused, ledger_t new_ledger __unused)
{
	spl_t                   s;
	processor_t             processor;
	ledger_t old_ledger = thread->t_bankledger;
	int64_t ctime, effective_ledger_time_consumed = 0;
	int64_t remainder = 0, consumed = 0;
	int64_t effective_energy_consumed = 0;
	uint64_t thread_energy;

	if (old_ledger == LEDGER_NULL && new_ledger == LEDGER_NULL) {
		return;
	}

	assert((thread == current_thread() || thread->started == 0));

	s = splsched();
	thread_lock(thread);

	/*
	 * Calculation of time elapsed by the thread before voucher swap.
	 * Following is the timeline which shows all the variables used in the calculation below.
	 *
	 *               thread ledger
	 *                 cpu_time
	 *                    |<-          consumed            ->|<- remainder  ->|
	 * timeline  ----------------------------------------------------------------->
	 *                    |                                  |                |
	 *             thread_dispatch                        ctime           quantum end
	 *
	 *                           |<-effective_ledger_time -> |
	 *               deduct_bank_ledger_time
	 */

	ctime = mach_absolute_time();
	processor = thread->last_processor;
	if (processor != NULL) {
		if ((int64_t)processor->quantum_end > ctime) {
			remainder = (int64_t)processor->quantum_end - ctime;
		}

		consumed = thread->quantum_remaining - remainder;
		effective_ledger_time_consumed = consumed - thread->t_deduct_bank_ledger_time;
	}

	thread->t_deduct_bank_ledger_time = consumed;

	thread_energy = recount_current_thread_energy_nj();
	effective_energy_consumed =
	    thread_energy - thread->t_deduct_bank_ledger_energy;
	assert(effective_energy_consumed >= 0);
	thread->t_deduct_bank_ledger_energy = thread_energy;

	thread->t_bankledger = new_ledger;

	thread_unlock(thread);
	splx(s);

	if (old_ledger != LEDGER_NULL) {
		ledger_credit(old_ledger,
		    bank_ledgers.cpu_time,
		    effective_ledger_time_consumed);
		ledger_credit(old_ledger,
		    bank_ledgers.energy,
		    effective_energy_consumed);
	}
}

/*
 * Routine: bank_verify_persona_id
 * Purpose: Verifies if the persona id is valid
 *
 * The caller should check if the task is entitled
 * to do the lookup.
 */
static boolean_t
bank_verify_persona_id(uint32_t persona_id, struct bank_persona *persona_out)
{
	/* A successful lookup implies that the persona id is valid */
	void *persona = persona_lookup(persona_id);
	if (!persona) {
		return FALSE;
	}

	persona_out->persona_id = persona_id;
	persona_out->persona_uid = persona_get_uid(persona);

	persona_put(persona);

	return TRUE;
}

static boolean_t
bank_task_can_adopt_persona(bank_task_t bank_merchant, struct bank_persona *persona)
{
	if (bank_merchant->bt_persona_id == persona->persona_id) {
		return TRUE;
	}

	if ((bank_merchant->bt_flags & PROC_PERSONA_INFO_FLAG_ADOPTION_ALLOWED) == 0) {
		return FALSE;
	}

	/*
	 * If persona UID is set, and it's not a System session persona that doesn't
	 * belong to any user (UINT_MAX), new persona UID needs to match the old
	 * one.
	 */
	uint32_t own_uid = bank_merchant->bt_persona_uid;
	uint32_t new_uid = persona->persona_uid;
	return own_uid == KAUTH_UID_NONE || own_uid == UINT_MAX || own_uid == new_uid;
}

static void
bank_task_get_persona(bank_task_t bank_task, struct bank_persona *persona_out)
{
	persona_out->persona_id = bank_task->bt_persona_id;
	persona_out->persona_uid = bank_task->bt_persona_uid;
}
