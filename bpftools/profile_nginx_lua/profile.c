/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * Copyright (c) 2022 LG Electronics
 *
 * Based on profile(8) from BCC by Brendan Gregg.
 * 28-Dec-2021 Eunseon Lee Created this,
 * 17-Jul-2022 Yusheng Zheng modified this.
 */
#include <argp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <time.h>
#include <linux/perf_event.h>
#include <sys/stat.h>
#include <asm/unistd.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "profile.h"
#include "lua_stacks_helper.h"
#include "profile.skel.h"
#include "trace_helpers.h"
#include "uprobe_helpers.h"

/* This structure combines key_t and count which should be sorted together */
struct key_ext_t
{
	struct profile_key_t k;
	__u64 v;
};

bool exiting = false;
struct lua_stack_map *lua_bt_map = NULL;

static struct env
{
	pid_t pid;
	pid_t tid;
	__u64 ns_dev;
	__u64 ns_ino;
	bool user_stacks_only;
	bool kernel_stacks_only;
	// control lua user space stack trace
	bool disable_lua_user_trace;
	bool lua_user_stacks_only;
	int stack_storage_size;
	int stack_depth_limit;
	int perf_max_stack_depth;
	int duration;
	bool verbose;
	bool freq;
	int sample_freq;
	bool delimiter;
	bool include_idle;
	bool folded;
	int cpu;
} env = {
	.pid = -1,
	.tid = -1,
	.ns_dev = 0,
	.ns_ino = 0,
	.stack_storage_size = 8192,
	.stack_depth_limit = 15,
	.perf_max_stack_depth = 127,
	.duration = 3,
	.freq = 1,
	.sample_freq = 49,
	.cpu = -1,
};

#define warn(...) fprintf(stderr, __VA_ARGS__)
#define UPROBE_SIZE 3

const char *argp_program_version = "profile 0.1";
const char *argp_program_bug_address =
	"https://github.com/iovisor/bcc/tree/master/libbpf-tools";
const char argp_program_doc[] =
	"Profile CPU usage by sampling stack traces at a timed interval.\n"
	"\n"
	"USAGE: profile [OPTIONS...] [duration]\n"
	"EXAMPLES:\n"
	"    profile             # profile stack traces at 49 Hertz until Ctrl-C\n"
	"    profile -F 99       # profile stack traces at 99 Hertz\n"
	"    profile -c 1000000  # profile stack traces every 1 in a million events\n"
	"    profile 5           # profile at 49 Hertz for 5 seconds only\n"
	"    profile -f          # output in folded format for flame graphs\n"
	"    profile -p 185      # only profile process with PID 185\n"
	"    profile -L 185      # only profile thread with TID 185\n"
	"    profile -U          # only show user space stacks (no kernel)\n"
	"    profile -K          # only show kernel space stacks (no user)\n";

#define OPT_PERF_MAX_STACK_DEPTH 1 /* --perf-max-stack-depth */
#define OPT_STACK_STORAGE_SIZE 2   /* --stack-storage-size */
#define OPT_STACK_DEPTH_LIMIT 3    /* --stack-depth-limit */
#define OPT_LUA_USER_STACK_ONLY 4  /* --lua-user-stacks-only */
#define OPT_DISABLE_LUA_USER_TRACE 5  /* --disable-lua-user-trace */
#define PERF_BUFFER_PAGES 16
#define PERF_POLL_TIMEOUT_MS 100

static const struct argp_option opts[] = {
	{"pid", 'p', "PID", 0, "profile process with this PID only"},
	{"tid", 'L', "TID", 0, "profile thread with this TID only"},
	{"user-stacks-only", 'U', NULL, 0,
	 "show stacks from user space only (no kernel space stacks)"},
	{"kernel-stacks-only", 'K', NULL, 0,
	 "show stacks from kernel space only (no user space stacks)"},
	{"lua-user-stacks-only", OPT_LUA_USER_STACK_ONLY, NULL, 0,
	 "replace user stacks with lua stack traces (no other user space stacks)"},
	{"disable-lua-user-trace", OPT_DISABLE_LUA_USER_TRACE, NULL, 0,
	 "disable lua user space stack trace"},
	{"frequency", 'F', "FREQUENCY", 0, "sample frequency, Hertz"},
	{"delimited", 'd', NULL, 0, "insert delimiter between kernel/user stacks"},
	{"include-idle ", 'I', NULL, 0, "include CPU idle stacks"},
	{"folded", 'f', NULL, 0, "output folded format, one line per stack (for flame graphs)"},
	{"stack-storage-size", OPT_STACK_STORAGE_SIZE, "STACK-STORAGE-SIZE", 0,
	 "the number of unique stack traces that can be stored and displayed (default 1024)"},
	{"stack-depth-limit", OPT_STACK_DEPTH_LIMIT, "OPT_STACK_DEPTH_LIMIT", 0,
	 "the limit depth of stack that be traversed (default 15)"},
	{"cpu", 'C', "CPU", 0, "cpu number to run profile on"},
	{"perf-max-stack-depth", OPT_PERF_MAX_STACK_DEPTH,
	 "PERF-MAX-STACK-DEPTH", 0, "the limit for both kernel and user stack traces (default 127)"},
	{"verbose", 'v', NULL, 0, "Verbose debug output"},
	{NULL, 'h', NULL, OPTION_HIDDEN, "Show the full help"},
	{},
};

static int read_ns_dev_ino(	__u64 *ns_dev, __u64 *ns_ino)
{
	struct stat statbuf;
    const char *path = "/proc/self/ns/pid";

    if (stat(path, &statbuf) == -1) {
        perror("stat");
        return 1;
    }

	*ns_dev = statbuf.st_dev;
	*ns_ino = statbuf.st_ino;

	return 0;
}

static error_t parse_arg(int key, char *arg, struct argp_state *state)
{
	static int pos_args;

	switch (key)
	{
	case 'h':
		argp_state_help(state, stderr, ARGP_HELP_STD_HELP);
		break;
	case 'v':
		env.verbose = true;
		break;
	case 'p':
		errno = 0;
		env.pid = strtol(arg, NULL, 10);
		if (errno)
		{
			fprintf(stderr, "invalid PID: %s\n", arg);
			argp_usage(state);
		}
		break;
	case 'L':
		errno = 0;
		env.tid = strtol(arg, NULL, 10);
		if (errno || env.tid <= 0)
		{
			fprintf(stderr, "Invalid TID: %s\n", arg);
			argp_usage(state);
		}
		break;
	case 'U':
		env.user_stacks_only = true;
		break;
	case 'K':
		env.kernel_stacks_only = true;
		break;
	case 'F':
		errno = 0;
		env.sample_freq = strtol(arg, NULL, 10);
		if (errno || env.sample_freq <= 0)
		{
			fprintf(stderr, "invalid FREQUENCY: %s\n", arg);
			argp_usage(state);
		}
		break;
	case 'd':
		env.delimiter = true;
		break;
	case 'I':
		env.include_idle = true;
		break;
	case 'f':
		env.folded = true;
		break;
	case 'C':
		errno = 0;
		env.cpu = strtol(arg, NULL, 10);
		if (errno)
		{
			fprintf(stderr, "invalid CPU: %s\n", arg);
			argp_usage(state);
		}
		break;
	case OPT_PERF_MAX_STACK_DEPTH:
		errno = 0;
		env.perf_max_stack_depth = strtol(arg, NULL, 10);
		if (errno)
		{
			fprintf(stderr, "invalid perf max stack depth: %s\n", arg);
			argp_usage(state);
		}
		break;
	case OPT_STACK_STORAGE_SIZE:
		errno = 0;
		env.stack_storage_size = strtol(arg, NULL, 10);
		if (errno)
		{
			fprintf(stderr, "invalid stack storage size: %s\n", arg);
			argp_usage(state);
		}
		break;
	case OPT_STACK_DEPTH_LIMIT:
		errno = 0;
		env.stack_depth_limit = strtol(arg, NULL, 10);
		if (errno)
		{
			fprintf(stderr, "invalid stack depth limit: %s\n", arg);
			argp_usage(state);
		}
		break;
	case OPT_LUA_USER_STACK_ONLY:
		env.lua_user_stacks_only = true;
		break;
	case OPT_DISABLE_LUA_USER_TRACE:
		env.disable_lua_user_trace = true;
		break;
	case ARGP_KEY_ARG:
		if (pos_args++)
		{
			fprintf(stderr,
					"Unrecognized positional argument: %s\n", arg);
			argp_usage(state);
		}
		errno = 0;
		env.duration = strtol(arg, NULL, 10);
		if (errno || env.duration <= 0)
		{
			fprintf(stderr, "Invalid duration (in s): %s\n", arg);
			argp_usage(state);
		}
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static int nr_cpus;

static int open_and_attach_perf_event(int freq, struct bpf_program *prog,
									  struct bpf_link *links[])
{
	struct perf_event_attr attr = {
		.type = PERF_TYPE_SOFTWARE,
		.freq = env.freq,
		.sample_freq = env.sample_freq,
		.config = PERF_COUNT_SW_CPU_CLOCK,
	};
	int i, fd;

	for (i = 0; i < nr_cpus; i++)
	{
		if (env.cpu != -1 && env.cpu != i)
			continue;

		fd = syscall(__NR_perf_event_open, &attr, -1, i, -1, 0);
		if (fd < 0)
		{
			/* Ignore CPU that is offline */
			if (errno == ENODEV)
				continue;
			fprintf(stderr, "failed to init perf sampling: %s\n",
					strerror(errno));
			return -1;
		}
		links[i] = bpf_program__attach_perf_event(prog, fd);
		if (!links[i])
		{
			fprintf(stderr, "failed to attach perf event on cpu: "
							"%d\n",
					i);
			links[i] = NULL;
			close(fd);
			return -1;
		}
	}

	return 0;
}

static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args)
{
	if (level == LIBBPF_DEBUG && !env.verbose)
		return 0;
	return vfprintf(stderr, format, args);
}

static void sig_handler(int sig)
{
	exiting = true;
}

static int stack_id_err(int stack_id)
{
	return (stack_id < 0) && (stack_id != -EFAULT);
}

static int cmp_counts(const void *dx, const void *dy)
{
	__u64 x = ((struct key_ext_t *)dx)->v;
	__u64 y = ((struct key_ext_t *)dy)->v;
	return x > y ? -1 : !(x == y);
}

static bool batch_map_ops = true; /* hope for the best */

static bool read_batch_counts_map(int fd, struct key_ext_t *items, __u32 *count)
{
	void *in = NULL, *out;
	__u32 i, n, n_read = 0;
	int err = 0;
	__u64 vals[*count];
	struct profile_key_t keys[*count];

	while (n_read < *count && !err)
	{
		n = *count - n_read;
		err = bpf_map_lookup_batch(fd, &in, &out, keys + n_read,
								   vals + n_read, &n, NULL);
		if (err && errno != ENOENT)
		{
			/* we want to propagate EINVAL upper, so that
			 * the batch_map_ops flag is set to false */
			if (errno != EINVAL)
				warn("bpf_map_lookup_batch: %s\n",
					 strerror(-err));
			return false;
		}
		n_read += n;
		in = out;
	}

	for (i = 0; i < n_read; i++)
	{
		items[i].k.pid = keys[i].pid;
		items[i].k.kernel_ip = keys[i].kernel_ip;
		items[i].k.user_stack_id = keys[i].user_stack_id;
		items[i].k.kern_stack_id = keys[i].kern_stack_id;
		strncpy(items[i].k.name, keys[i].name, TASK_COMM_LEN);
		items[i].v = vals[i];
	}

	*count = n_read;
	return true;
}

static bool read_counts_map(int fd, struct key_ext_t *items, __u32 *count)
{
	struct profile_key_t empty = {};
	struct profile_key_t *lookup_key = &empty;
	int i = 0;
	int err;

	if (batch_map_ops)
	{
		bool ok = read_batch_counts_map(fd, items, count);
		if (!ok && errno == EINVAL)
		{
			/* fall back to a racy variant */
			batch_map_ops = false;
		}
		else
		{
			return ok;
		}
	}

	if (!items || !count || !*count)
		return true;

	while (!bpf_map_get_next_key(fd, lookup_key, &items[i].k))
	{

		err = bpf_map_lookup_elem(fd, &items[i].k, &items[i].v);
		if (err < 0)
		{
			fprintf(stderr, "failed to lookup counts: %d\n", err);
			return false;
		}
		if (items[i].v == 0)
			continue;

		lookup_key = &items[i].k;
		i++;
	}

	*count = i;
	return true;
}

static void print_fold_lua_func(const struct syms *syms, const struct lua_stack_event *eventp)
{
	if (!eventp)
	{
		return;
	}
	if (eventp->type == FUNC_TYPE_LUA)
	{
		if (eventp->ffid)
		{
			printf(";L:%s:%d", eventp->name, eventp->ffid);
		}
		else
		{
			printf(";L:%s", eventp->name);
		}
	}
	else if (eventp->type == FUNC_TYPE_C)
	{
		const struct sym *sym = syms__map_addr(syms, (unsigned long)eventp->funcp);
		if (sym)
		{
			printf(";C:%s", sym ? sym->name : "[unknown]");
		}
	}
	else if (eventp->type == FUNC_TYPE_F)
	{
		printf(";builtin#%d", eventp->ffid);
	}
	else
	{
		printf(";[unknown]");
	}
}

static void print_fold_user_stack_with_lua(const struct stack_backtrace *lua_bt, const struct syms *syms, unsigned long *uip, unsigned int nr_uip)
{
	const struct sym *sym = NULL;
	int lua_bt_count = lua_bt->level_size - 1;
	for (int j = nr_uip - 1; j >= 0; j--)
	{
		sym = syms__map_addr(syms, uip[j]);
		if (sym)
		{
			if (!env.lua_user_stacks_only)
			{
				printf(";%s", sym->name);
			}
		}
		else
		{
			if (lua_bt_count >= 0)
			{
				print_fold_lua_func(syms, &(lua_bt->stack[lua_bt_count]));
				lua_bt_count--;
			}
		}
	}
	while (lua_bt_count >= 0)
	{
		print_fold_lua_func(syms, &(lua_bt->stack[lua_bt_count]));
		lua_bt_count--;
	}
}

static void print_map(struct ksyms *ksyms, struct syms_cache *syms_cache,
					  struct profile_bpf *obj)
{
	const struct ksym *ksym;
	const struct syms *syms = NULL;
	const struct sym *sym;
	int i, j, cfd, sfd;
	struct stack_backtrace lua_bt = {0};
	__u32 nr_count;
	struct profile_key_t *k;
	__u64 v;
	unsigned long *kip;
	unsigned long *uip;
	bool has_collision = false;
	unsigned int missing_stacks = 0;
	struct key_ext_t counts[MAX_ENTRIES];
	unsigned int nr_kip;
	unsigned int nr_uip;
	int idx = 0;

	/* add 1 for kernel_ip */
	kip = calloc(env.perf_max_stack_depth + 1, sizeof(*kip));
	if (!kip)
	{
		fprintf(stderr, "failed to alloc kernel ip\n");
		return;
	}

	uip = calloc(env.perf_max_stack_depth, sizeof(*uip));
	if (!uip)
	{
		fprintf(stderr, "failed to alloc user ip\n");
		return;
	}

	cfd = bpf_map__fd(obj->maps.counts);
	sfd = bpf_map__fd(obj->maps.stackmap);

	nr_count = MAX_ENTRIES;
	if (!read_counts_map(cfd, counts, &nr_count))
	{
		goto cleanup;
	}

	qsort(counts, nr_count, sizeof(counts[0]), cmp_counts);

	for (i = 0; i < nr_count; i++)
	{
		k = &counts[i].k;
		v = counts[i].v;
		nr_uip = 0;
		nr_kip = 0;
		idx = 0;

		if (!env.user_stacks_only && stack_id_err(k->kern_stack_id))
		{
			missing_stacks += 1;
			has_collision |= (k->kern_stack_id == -EEXIST);
		}
		if (!env.kernel_stacks_only && stack_id_err(k->user_stack_id))
		{
			missing_stacks += 1;
			has_collision |= (k->user_stack_id == -EEXIST);
		}

		if (!env.kernel_stacks_only && k->user_stack_id >= 0)
		{
			if (bpf_map_lookup_elem(sfd, &k->user_stack_id, uip) == 0)
			{
				/* count the number of ips */
				while (nr_uip < env.perf_max_stack_depth && uip[nr_uip])
					nr_uip++;
				syms = syms_cache__get_syms(syms_cache, k->pid);
			}
			int stack_level = get_lua_stack_backtrace(lua_bt_map, k->user_stack_id, &lua_bt);
			if (env.lua_user_stacks_only && env.folded) {
				if (stack_level <= 0) {
					// if show lua user stack only, then we do not count the stack if it is not lua stack
					continue;
				}
			}
		}

		if (!env.user_stacks_only && k->kern_stack_id >= 0)
		{
			if (k->kernel_ip)
				kip[nr_kip++] = k->kernel_ip;
			if (bpf_map_lookup_elem(sfd, &k->kern_stack_id, kip + nr_kip) == 0)
			{
				/* count the number of ips */
				while (nr_kip < env.perf_max_stack_depth && kip[nr_kip])
					nr_kip++;
			}
		}

		if (env.folded)
		{
			// print folded stack output
			printf("%s", k->name);

			if (!env.kernel_stacks_only)
			{
				if (stack_id_err(k->user_stack_id))
					printf(";[Missed User Stack]");
				if (syms)
				{
					if (!env.disable_lua_user_trace)
					{
						print_fold_user_stack_with_lua(&lua_bt, syms, uip, nr_uip);
					}
					else
					{
						const struct sym *sym = NULL;
						for (int j = nr_uip - 1; j >= 0; j--)
						{
							sym = syms__map_addr(syms, uip[j]);
							printf(";%s", sym ? sym->name : "[unknown]");
						}
					}
				}
			}
			if (!env.user_stacks_only)
			{
				if (env.delimiter && k->user_stack_id >= 0 &&
					k->kern_stack_id >= 0)
					printf(";-");

				if (stack_id_err(k->kern_stack_id))
					printf(";[Missed Kernel Stack]");
				for (j = nr_kip - 1; j >= 0; j--)
				{
					ksym = ksyms__map_addr(ksyms, kip[j]);
					printf(";%s", ksym ? ksym->name : "[unknown]");
				}
			}
			printf(" %lld\n", v);
		}
		else
		{
			// print default multi-line stack output
			if (!env.user_stacks_only)
			{
				if (stack_id_err(k->kern_stack_id))
					printf("    [Missed Kernel Stack]\n");
				for (j = 0; j < nr_kip; j++)
				{
					ksym = ksyms__map_addr(ksyms, kip[j]);
					if (ksym)
						printf("    #%-2d 0x%lx %s+0x%lx\n", idx++, kip[j], ksym->name, kip[j] - ksym->addr);
					else
						printf("    #%-2d 0x%lx [unknown]\n", idx++, kip[j]);
				}
			}

			if (!env.kernel_stacks_only)
			{
				if (env.delimiter && k->kern_stack_id >= 0 &&
					k->user_stack_id >= 0)
					printf("    --\n");

				if (stack_id_err(k->user_stack_id))
					printf("    [Missed User Stack]\n");
				if (!syms)
				{
					for (j = 0; j < nr_uip; j++)
						printf("    #%-2d 0x%016lx [unknown]\n", idx++, uip[j]);
				}
				else
				{
					for (j = 0; j < nr_uip; j++)
					{
						char *dso_name;
						uint64_t dso_offset;
						sym = syms__map_addr_dso(syms, uip[j], &dso_name, &dso_offset);

						printf("    #%-2d 0x%016lx", idx++, uip[j]);
						if (sym)
							printf(" %s+0x%lx", sym->name, sym->offset);
						if (dso_name)
							printf(" (%s+0x%lx)", dso_name, dso_offset);
						printf("\n");
					}
				}
			}

			printf("    %-16s %s (%d)\n", "-", k->name, k->pid);
			printf("        %lld\n\n", v);
		}
	}

	if (missing_stacks > 0)
	{
		fprintf(stderr, "WARNING: %d stack traces could not be displayed.%s\n",
				missing_stacks, has_collision ? " Consider increasing --stack-storage-size." : "");
	}

cleanup:
	free(kip);
	free(uip);
}

static void handle_lua_stack_event(void *ctx, int cpu, void *data, __u32 data_sz)
{
	int err;
	const struct lua_stack_event *e = data;
	struct tm *tm;
	char ts[16];
	time_t t;

	time(&t);
	tm = localtime(&t);
	strftime(ts, sizeof(ts), "%H:%M:%S", tm);
	err = insert_lua_stack_map(lua_bt_map, e);
	if (err)
		fprintf(stderr, "failed to insert lua stack map\n");
}

static void handle_lua_stack_lost_events(void *ctx, int cpu, __u64 lost_cnt)
{
	warn("lost %llu events on CPU #%d\n", lost_cnt, cpu);
}

static int attach_lua_uprobes(struct profile_bpf *obj, struct bpf_link *links[])
{
	char lua_path[128];
	if (env.pid)
	{
		int res = 0;

		res = get_pid_lib_path(env.pid, "luajit-5.1.so", lua_path, sizeof(lua_path));
		if (res < 0)
		{
			fprintf(stderr, "failed to get lib path for pid %d\n", env.pid);
			return -1;
		}
	}

	off_t func_off = get_elf_func_offset(lua_path, "lua_resume");
	if (func_off < 0)
	{
		warn("could not find lua_resume in %s\n", lua_path);
		return -1;
	}
	links[0] = bpf_program__attach_uprobe(obj->progs.handle_entry_lua, false,
										  -1, lua_path, func_off);
	if (!links[0])
	{
		warn("failed to attach lua_resume: %d\n", -errno);
		return -1;
	}

	func_off = get_elf_func_offset(lua_path, "lua_pcall");
	if (func_off < 0)
	{
		warn("could not find lua_pcall in %s\n", lua_path);
		return -1;
	}
	links[1] = bpf_program__attach_uprobe(obj->progs.handle_entry_lua, false,
										  -1, lua_path, func_off);
	if (!links[1])
	{
		warn("failed to attach lua_pcall: %d\n", -errno);
		return -1;
	}

	func_off = get_elf_func_offset(lua_path, "lua_yield");
	if (func_off < 0)
	{
		warn("could not find lua_yield in %s\n", lua_path);
		return -1;
	}
	links[2] = bpf_program__attach_uprobe(obj->progs.handle_entry_lua_cancel, false,
										  -1, lua_path, func_off);
	if (!links[2])
	{
		warn("failed to attach lua_yield: %d\n", -errno);
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	static const struct argp argp = {
		.options = opts,
		.parser = parse_arg,
		.doc = argp_program_doc,
	};
	struct syms_cache *syms_cache = NULL;
	struct ksyms *ksyms = NULL;
	struct perf_buffer *pb = NULL;
	struct bpf_link *cpu_links[MAX_CPU_NR] = {};
	struct bpf_link *uprobe_links[UPROBE_SIZE] = {};
	struct profile_bpf *obj;
	int err, i;
	char *stack_context = "user + kernel";
	char thread_context[64];
	char sample_context[64];

	err = argp_parse(&argp, argc, argv, 0, NULL, NULL);
	if (err)
		return err;
	if (env.user_stacks_only && env.kernel_stacks_only)
	{
		fprintf(stderr, "user_stacks_only and kernel_stacks_only cannot be used together.\n");
		return 1;
	}

	libbpf_set_print(libbpf_print_fn);
	libbpf_set_strict_mode(LIBBPF_STRICT_ALL);

	nr_cpus = libbpf_num_possible_cpus();
	if (nr_cpus < 0)
	{
		printf("failed to get # of possible cpus: '%s'!\n",
			   strerror(-nr_cpus));
		return 1;
	}
	if (nr_cpus > MAX_CPU_NR)
	{
		fprintf(stderr, "the number of cpu cores is too big, please "
						"increase MAX_CPU_NR's value and recompile");
		return 1;
	}

	obj = profile_bpf__open();
	if (!obj)
	{
		fprintf(stderr, "failed to open BPF object\n");
		return 1;
	}

	if(read_ns_dev_ino(&env.ns_dev, &env.ns_ino))
	{
		fprintf(stderr, "failed to read ns_dev and ns_ino\n");
		return 1;
	}

	/* initialize global data (filtering options) */
	obj->rodata->targ_pid = env.pid;
	obj->rodata->targ_tid = env.tid;
	obj->rodata->targ_ns_dev = env.ns_dev;
	obj->rodata->targ_ns_ino = env.ns_ino;
	obj->rodata->stack_depth_limit = env.stack_depth_limit;
	obj->rodata->user_stacks_only = env.user_stacks_only;
	obj->rodata->kernel_stacks_only = env.kernel_stacks_only;
	obj->rodata->include_idle = env.include_idle;

	bpf_map__set_value_size(obj->maps.stackmap,
							env.perf_max_stack_depth * sizeof(unsigned long));
	bpf_map__set_max_entries(obj->maps.stackmap, env.stack_storage_size);

	err = profile_bpf__load(obj);
	if (err)
	{
		fprintf(stderr, "failed to load BPF programs. "
						"if the error message indicates `BPF program is too large`, "
						"consider using the `--stack-depth-limit` option.\n");
		goto cleanup;
	}
	ksyms = ksyms__load();
	if (!ksyms)
	{
		fprintf(stderr, "failed to load kallsyms\n");
		goto cleanup;
	}
	syms_cache = syms_cache__new(0);
	if (!syms_cache)
	{
		fprintf(stderr, "failed to create syms_cache\n");
		goto cleanup;
	}

	err = attach_lua_uprobes(obj, uprobe_links);
	if (err < 0)
	{
		// cannot found lua lib, so skip lua uprobe
		env.disable_lua_user_trace = true;
	}

	lua_bt_map = init_lua_stack_map();
	if (!lua_bt_map)
		goto cleanup;
	pb = perf_buffer__new(bpf_map__fd(obj->maps.lua_event_output), PERF_BUFFER_PAGES,
									handle_lua_stack_event, handle_lua_stack_lost_events, NULL, NULL);
	if (!pb)
	{
		err = -errno;
		warn("failed to open perf buffer: %d\n", err);
		goto cleanup;
	}

	err = open_and_attach_perf_event(env.freq, obj->progs.do_perf_event, cpu_links);
	if (err)
		goto cleanup;

	signal(SIGINT, sig_handler);

	if (env.pid != -1)
		snprintf(thread_context, sizeof(thread_context), "PID %d", env.pid);
	else if (env.tid != -1)
		snprintf(thread_context, sizeof(thread_context), "TID %d", env.tid);
	else
		snprintf(thread_context, sizeof(thread_context), "all threads");

	snprintf(sample_context, sizeof(sample_context), "%d Hertz", env.sample_freq);

	if (env.user_stacks_only)
		stack_context = "user";
	else if (env.kernel_stacks_only)
		stack_context = "kernel";

	if (!env.folded)
	{
		printf("Sampling at %s of %s by %s stack", sample_context, thread_context, stack_context);
		if (env.cpu != -1)
			printf(" on CPU#%d", env.cpu);
		if (env.duration < 99999999)
			printf(" for %d secs.\n", env.duration);
		else
			printf("... Hit Ctrl-C to end.\n");
	}

	/*
	 * We'll get sleep interrupted when someone presses Ctrl-C (which will
	 * be "handled" with noop by sig_handler).
	 */
	time_t start_time = time(NULL);
	while (!exiting)
	{
		if (env.duration > 0 && (time(NULL) - start_time) >= env.duration) {
			// printf("Terminating after %d seconds.\n", env.duration);
			break;
	    }
		// print perf event to get stack trace
		err = perf_buffer__poll(pb, PERF_POLL_TIMEOUT_MS);
		if (err < 0 && err != -EINTR)
		{
			warn("error polling perf buffer: %s\n", strerror(-err));
			goto cleanup;
		}
		/* reset err to return 0 if exiting */
		err = 0;
	}

	print_map(ksyms, syms_cache, obj);

cleanup:
	if (env.cpu != -1)
		bpf_link__destroy(cpu_links[env.cpu]);
	else
	{
		for (i = 0; i < nr_cpus; i++)
			bpf_link__destroy(cpu_links[i]);
	}
	for (i = 0; i < UPROBE_SIZE; i++)
		bpf_link__destroy(uprobe_links[i]);
	profile_bpf__destroy(obj);

	if (pb) perf_buffer__free(pb);
	if (syms_cache) syms_cache__free(syms_cache);
	if (ksyms) ksyms__free(ksyms);

	return err != 0;
}
