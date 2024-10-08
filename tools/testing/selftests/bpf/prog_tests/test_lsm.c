// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (C) 2020 Google LLC.
 */

#include <test_progs.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <malloc.h>
#include <stdlib.h>

#include "lsm.skel.h"
#include "lsm_tailcall.skel.h"

char *CMD_ARGS[] = {"true", NULL};

#define GET_PAGE_ADDR(ADDR, PAGE_SIZE)					\
	(char *)(((unsigned long) (ADDR + PAGE_SIZE)) & ~(PAGE_SIZE-1))

int stack_mprotect(void)
{
	void *buf;
	long sz;
	int ret;

	sz = sysconf(_SC_PAGESIZE);
	if (sz < 0)
		return sz;

	buf = alloca(sz * 3);
	ret = mprotect(GET_PAGE_ADDR(buf, sz), sz,
		       PROT_READ | PROT_WRITE | PROT_EXEC);
	return ret;
}

int exec_cmd(int *monitored_pid)
{
	int child_pid, child_status;

	child_pid = fork();
	if (child_pid == 0) {
		*monitored_pid = getpid();
		execvp(CMD_ARGS[0], CMD_ARGS);
		return -EINVAL;
	} else if (child_pid > 0) {
		waitpid(child_pid, &child_status, 0);
		return child_status;
	}

	return -EINVAL;
}

static int test_lsm(struct lsm *skel)
{
	struct bpf_link *link;
	int buf = 1234;
	int err;

	err = lsm__attach(skel);
	if (!ASSERT_OK(err, "attach"))
		return err;

	/* Check that already linked program can't be attached again. */
	link = bpf_program__attach(skel->progs.test_int_hook);
	if (!ASSERT_ERR_PTR(link, "attach_link"))
		return -1;

	err = exec_cmd(&skel->bss->monitored_pid);
	if (!ASSERT_OK(err, "exec_cmd"))
		return err;

	ASSERT_EQ(skel->bss->bprm_count, 1, "bprm_count");

	skel->bss->monitored_pid = getpid();

	err = stack_mprotect();
	if (!ASSERT_EQ(err, -1, "stack_mprotect") ||
	    !ASSERT_EQ(errno, EPERM, "stack_mprotect"))
		return err;

	ASSERT_EQ(skel->bss->mprotect_count, 1, "mprotect_count");

	syscall(__NR_setdomainname, &buf, -2L);
	syscall(__NR_setdomainname, 0, -3L);
	syscall(__NR_setdomainname, ~0L, -4L);

	ASSERT_EQ(skel->bss->copy_test, 3, "copy_test");

	lsm__detach(skel);

	skel->bss->copy_test = 0;
	skel->bss->bprm_count = 0;
	skel->bss->mprotect_count = 0;
	return 0;
}

static void test_lsm_basic(void)
{
	struct lsm *skel = NULL;
	int err;

	skel = lsm__open_and_load();
	if (!ASSERT_OK_PTR(skel, "lsm_skel_load"))
		goto close_prog;

	err = test_lsm(skel);
	if (!ASSERT_OK(err, "test_lsm_first_attach"))
		goto close_prog;

	err = test_lsm(skel);
	ASSERT_OK(err, "test_lsm_second_attach");

close_prog:
	lsm__destroy(skel);
}

static void test_lsm_tailcall(void)
{
	struct lsm_tailcall *skel = NULL;
	int map_fd, prog_fd;
	int err, key;

	skel = lsm_tailcall__open_and_load();
	if (!ASSERT_OK_PTR(skel, "lsm_tailcall__skel_load"))
		goto close_prog;

	map_fd = bpf_map__fd(skel->maps.jmp_table);
	if (CHECK_FAIL(map_fd < 0))
		goto close_prog;

	prog_fd = bpf_program__fd(skel->progs.lsm_file_permission_prog);
	if (CHECK_FAIL(prog_fd < 0))
		goto close_prog;

	key = 0;
	err = bpf_map_update_elem(map_fd, &key, &prog_fd, BPF_ANY);
	if (CHECK_FAIL(!err))
		goto close_prog;

	prog_fd = bpf_program__fd(skel->progs.lsm_file_alloc_security_prog);
	if (CHECK_FAIL(prog_fd < 0))
		goto close_prog;

	err = bpf_map_update_elem(map_fd, &key, &prog_fd, BPF_ANY);
	if (CHECK_FAIL(err))
		goto close_prog;

close_prog:
	lsm_tailcall__destroy(skel);
}

void test_test_lsm(void)
{
	if (test__start_subtest("lsm_basic"))
		test_lsm_basic();
	if (test__start_subtest("lsm_tailcall"))
		test_lsm_tailcall();
}
