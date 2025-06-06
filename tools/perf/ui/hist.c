// SPDX-License-Identifier: GPL-2.0
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <linux/compiler.h>

#include "../util/callchain.h"
#include "../util/debug.h"
#include "../util/hist.h"
#include "../util/sort.h"
#include "../util/evsel.h"
#include "../util/evlist.h"
#include "../util/mem-events.h"
#include "../util/string2.h"
#include "../util/thread.h"
#include "../util/util.h"

/* hist period print (hpp) functions */

#define hpp__call_print_fn(hpp, fn, fmt, ...)			\
({								\
	int __ret = fn(hpp, fmt, ##__VA_ARGS__);		\
	advance_hpp(hpp, __ret);				\
	__ret;							\
})

static int __hpp__fmt_print(struct perf_hpp *hpp, struct hists *hists, u64 val,
			    int nr_samples, const char *fmt, int len,
			    hpp_snprint_fn print_fn, enum perf_hpp_fmt_type fmtype)
{
	if (fmtype == PERF_HPP_FMT_TYPE__PERCENT || fmtype == PERF_HPP_FMT_TYPE__LATENCY) {
		double percent = 0.0;
		u64 total = fmtype == PERF_HPP_FMT_TYPE__PERCENT ? hists__total_period(hists) :
			hists__total_latency(hists);

		if (total)
			percent = 100.0 * val / total;

		return hpp__call_print_fn(hpp, print_fn, fmt, len, percent);
	}

	if (fmtype == PERF_HPP_FMT_TYPE__AVERAGE) {
		double avg = nr_samples ? (1.0 * val / nr_samples) : 0;

		return hpp__call_print_fn(hpp, print_fn, fmt, len, avg);
	}

	return hpp__call_print_fn(hpp, print_fn, fmt, len, val);
}

struct hpp_fmt_value {
	struct hists *hists;
	u64 val;
	int samples;
};

static int __hpp__fmt(struct perf_hpp *hpp, struct hist_entry *he,
		      hpp_field_fn get_field, const char *fmt, int len,
		      hpp_snprint_fn print_fn, enum perf_hpp_fmt_type fmtype)
{
	int ret = 0;
	struct hists *hists = he->hists;
	struct evsel *evsel = hists_to_evsel(hists);
	struct evsel *pos;
	char *buf = hpp->buf;
	size_t size = hpp->size;
	int i = 0, nr_members = 1;
	struct hpp_fmt_value *values;

	if (evsel__is_group_event(evsel))
		nr_members = evsel->core.nr_members;

	values = calloc(nr_members, sizeof(*values));
	if (values == NULL)
		return 0;

	values[0].hists = evsel__hists(evsel);
	values[0].val = get_field(he);
	values[0].samples = he->stat.nr_events;

	if (evsel__is_group_event(evsel)) {
		struct hist_entry *pair;

		for_each_group_member(pos, evsel)
			values[++i].hists = evsel__hists(pos);

		list_for_each_entry(pair, &he->pairs.head, pairs.node) {
			for (i = 0; i < nr_members; i++) {
				if (values[i].hists != pair->hists)
					continue;

				values[i].val = get_field(pair);
				values[i].samples = pair->stat.nr_events;
				break;
			}
		}
	}

	for (i = 0; i < nr_members; i++) {
		if (symbol_conf.skip_empty &&
		    values[i].hists->stats.nr_samples == 0)
			continue;

		ret += __hpp__fmt_print(hpp, values[i].hists, values[i].val,
					values[i].samples, fmt, len,
					print_fn, fmtype);
	}

	free(values);

	/*
	 * Restore original buf and size as it's where caller expects
	 * the result will be saved.
	 */
	hpp->buf = buf;
	hpp->size = size;

	return ret;
}

int hpp__fmt(struct perf_hpp_fmt *fmt, struct perf_hpp *hpp,
	     struct hist_entry *he, hpp_field_fn get_field,
	     const char *fmtstr, hpp_snprint_fn print_fn,
	     enum perf_hpp_fmt_type fmtype)
{
	int len = max(fmt->user_len ?: fmt->len, (int)strlen(fmt->name));

	if (symbol_conf.field_sep) {
		return __hpp__fmt(hpp, he, get_field, fmtstr, 1,
				  print_fn, fmtype);
	}

	if (fmtype == PERF_HPP_FMT_TYPE__PERCENT || fmtype == PERF_HPP_FMT_TYPE__LATENCY)
		len -= 2; /* 2 for a space and a % sign */
	else
		len -= 1;

	return  __hpp__fmt(hpp, he, get_field, fmtstr, len, print_fn, fmtype);
}

int hpp__fmt_acc(struct perf_hpp_fmt *fmt, struct perf_hpp *hpp,
		 struct hist_entry *he, hpp_field_fn get_field,
		 const char *fmtstr, hpp_snprint_fn print_fn,
		 enum perf_hpp_fmt_type fmtype)
{
	if (!symbol_conf.cumulate_callchain) {
		int len = fmt->user_len ?: fmt->len;
		return snprintf(hpp->buf, hpp->size, " %*s", len - 1, "N/A");
	}

	return hpp__fmt(fmt, hpp, he, get_field, fmtstr, print_fn, fmtype);
}

int hpp__fmt_mem_stat(struct perf_hpp_fmt *fmt __maybe_unused, struct perf_hpp *hpp,
		      struct hist_entry *he, enum mem_stat_type mst,
		      const char *fmtstr, hpp_snprint_fn print_fn)
{
	struct hists *hists = he->hists;
	int mem_stat_idx = -1;
	char *buf = hpp->buf;
	size_t size = hpp->size;
	u64 total = 0;
	int ret = 0;

	for (int i = 0; i < hists->nr_mem_stats; i++) {
		if (hists->mem_stat_types[i] == mst) {
			mem_stat_idx = i;
			break;
		}
	}
	assert(mem_stat_idx != -1);

	for (int i = 0; i < MEM_STAT_LEN; i++)
		total += hists->mem_stat_total[mem_stat_idx].entries[i];
	assert(total != 0);

	for (int i = 0; i < MEM_STAT_LEN; i++) {
		u64 val = he->mem_stat[mem_stat_idx].entries[i];

		if (hists->mem_stat_total[mem_stat_idx].entries[i] == 0)
			continue;

		ret += hpp__call_print_fn(hpp, print_fn, fmtstr, 100.0 * val / total);
	}

	/*
	 * Restore original buf and size as it's where caller expects
	 * the result will be saved.
	 */
	hpp->buf = buf;
	hpp->size = size;

	return ret;
}

static int field_cmp(u64 field_a, u64 field_b)
{
	if (field_a > field_b)
		return 1;
	if (field_a < field_b)
		return -1;
	return 0;
}

static int hist_entry__new_pair(struct hist_entry *a, struct hist_entry *b,
				hpp_field_fn get_field, int nr_members,
				u64 **fields_a, u64 **fields_b)
{
	u64 *fa = calloc(nr_members, sizeof(*fa)),
	    *fb = calloc(nr_members, sizeof(*fb));
	struct hist_entry *pair;

	if (!fa || !fb)
		goto out_free;

	list_for_each_entry(pair, &a->pairs.head, pairs.node) {
		struct evsel *evsel = hists_to_evsel(pair->hists);
		fa[evsel__group_idx(evsel)] = get_field(pair);
	}

	list_for_each_entry(pair, &b->pairs.head, pairs.node) {
		struct evsel *evsel = hists_to_evsel(pair->hists);
		fb[evsel__group_idx(evsel)] = get_field(pair);
	}

	*fields_a = fa;
	*fields_b = fb;
	return 0;
out_free:
	free(fa);
	free(fb);
	*fields_a = *fields_b = NULL;
	return -1;
}

static int __hpp__group_sort_idx(struct hist_entry *a, struct hist_entry *b,
				 hpp_field_fn get_field, int idx)
{
	struct evsel *evsel = hists_to_evsel(a->hists);
	u64 *fields_a, *fields_b;
	int cmp, nr_members, ret, i;

	cmp = field_cmp(get_field(a), get_field(b));
	if (!evsel__is_group_event(evsel))
		return cmp;

	nr_members = evsel->core.nr_members;
	if (idx < 1 || idx >= nr_members)
		return cmp;

	ret = hist_entry__new_pair(a, b, get_field, nr_members, &fields_a, &fields_b);
	if (ret) {
		ret = cmp;
		goto out;
	}

	ret = field_cmp(fields_a[idx], fields_b[idx]);
	if (ret)
		goto out;

	for (i = 1; i < nr_members; i++) {
		if (i != idx) {
			ret = field_cmp(fields_a[i], fields_b[i]);
			if (ret)
				goto out;
		}
	}

out:
	free(fields_a);
	free(fields_b);

	return ret;
}

static int __hpp__sort(struct hist_entry *a, struct hist_entry *b,
		       hpp_field_fn get_field)
{
	s64 ret;
	int i, nr_members;
	struct evsel *evsel;
	u64 *fields_a, *fields_b;

	if (symbol_conf.group_sort_idx && symbol_conf.event_group) {
		return __hpp__group_sort_idx(a, b, get_field,
					     symbol_conf.group_sort_idx);
	}

	ret = field_cmp(get_field(a), get_field(b));
	if (ret || !symbol_conf.event_group)
		return ret;

	evsel = hists_to_evsel(a->hists);
	if (!evsel__is_group_event(evsel))
		return ret;

	nr_members = evsel->core.nr_members;
	i = hist_entry__new_pair(a, b, get_field, nr_members, &fields_a, &fields_b);
	if (i)
		goto out;

	for (i = 1; i < nr_members; i++) {
		ret = field_cmp(fields_a[i], fields_b[i]);
		if (ret)
			break;
	}

out:
	free(fields_a);
	free(fields_b);

	return ret;
}

static int __hpp__sort_acc(struct hist_entry *a, struct hist_entry *b,
			   hpp_field_fn get_field)
{
	s64 ret = 0;

	if (symbol_conf.cumulate_callchain) {
		/*
		 * Put caller above callee when they have equal period.
		 */
		ret = field_cmp(get_field(a), get_field(b));
		if (ret)
			return ret;

		if ((a->thread == NULL ? NULL : RC_CHK_ACCESS(a->thread)) !=
		    (b->thread == NULL ? NULL : RC_CHK_ACCESS(b->thread)) ||
		    !hist_entry__has_callchains(a) || !symbol_conf.use_callchain)
			return 0;

		ret = b->callchain->max_depth - a->callchain->max_depth;
		if (callchain_param.order == ORDER_CALLER)
			ret = -ret;
	}
	return ret;
}

static bool perf_hpp__is_mem_stat_entry(struct perf_hpp_fmt *fmt);

static enum mem_stat_type hpp__mem_stat_type(struct perf_hpp_fmt *fmt)
{
	if (!perf_hpp__is_mem_stat_entry(fmt))
		return -1;

	switch (fmt->idx) {
	case PERF_HPP__MEM_STAT_OP:
		return PERF_MEM_STAT_OP;
	case PERF_HPP__MEM_STAT_CACHE:
		return PERF_MEM_STAT_CACHE;
	case PERF_HPP__MEM_STAT_MEMORY:
		return PERF_MEM_STAT_MEMORY;
	case PERF_HPP__MEM_STAT_SNOOP:
		return PERF_MEM_STAT_SNOOP;
	case PERF_HPP__MEM_STAT_DTLB:
		return PERF_MEM_STAT_DTLB;
	default:
		break;
	}
	pr_debug("Should not reach here\n");
	return -1;
}

static int64_t hpp__sort_mem_stat(struct perf_hpp_fmt *fmt __maybe_unused,
				  struct hist_entry *a, struct hist_entry *b)
{
	return a->stat.period - b->stat.period;
}

static int hpp__width_fn(struct perf_hpp_fmt *fmt,
			 struct perf_hpp *hpp __maybe_unused,
			 struct hists *hists)
{
	int len = fmt->user_len ?: fmt->len;
	struct evsel *evsel = hists_to_evsel(hists);

	if (symbol_conf.event_group) {
		int nr = 0;
		struct evsel *pos;

		for_each_group_evsel(pos, evsel) {
			if (!symbol_conf.skip_empty ||
			    evsel__hists(pos)->stats.nr_samples)
				nr++;
		}

		len = max(len, nr * fmt->len);
	}

	if (len < (int)strlen(fmt->name))
		len = strlen(fmt->name);

	return len;
}

static int hpp__header_fn(struct perf_hpp_fmt *fmt, struct perf_hpp *hpp,
			  struct hists *hists, int line,
			  int *span __maybe_unused)
{
	int len = hpp__width_fn(fmt, hpp, hists);
	const char *hdr = "";

	if (line == hists->hpp_list->nr_header_lines - 1)
		hdr = fmt->name;

	return scnprintf(hpp->buf, hpp->size, "%*s", len, hdr);
}

static int hpp__header_mem_stat_fn(struct perf_hpp_fmt *fmt, struct perf_hpp *hpp,
				   struct hists *hists, int line,
				   int *span __maybe_unused)
{
	char *buf = hpp->buf;
	int ret = 0;
	int len;
	enum mem_stat_type mst = hpp__mem_stat_type(fmt);
	int mem_stat_idx = -1;

	for (int i = 0; i < hists->nr_mem_stats; i++) {
		if (hists->mem_stat_types[i] == mst) {
			mem_stat_idx = i;
			break;
		}
	}
	assert(mem_stat_idx != -1);

	if (line == 0) {
		int left, right;

		len = 0;
		/* update fmt->len for acutally used columns only */
		for (int i = 0; i < MEM_STAT_LEN; i++) {
			if (hists->mem_stat_total[mem_stat_idx].entries[i])
				len += MEM_STAT_PRINT_LEN;
		}
		fmt->len = len;

		/* print header directly if single column only */
		if (len == MEM_STAT_PRINT_LEN)
			return scnprintf(hpp->buf, hpp->size, "%*s", len, fmt->name);

		left = (len - strlen(fmt->name)) / 2 - 1;
		right = len - left - strlen(fmt->name) - 2;

		if (left < 0)
			left = 0;
		if (right < 0)
			right = 0;

		return scnprintf(hpp->buf, hpp->size, "%.*s %s %.*s",
				 left, graph_dotted_line, fmt->name, right, graph_dotted_line);
	}


	len = hpp->size;
	for (int i = 0; i < MEM_STAT_LEN; i++) {
		int printed;

		if (hists->mem_stat_total[mem_stat_idx].entries[i] == 0)
			continue;

		printed = scnprintf(buf, len, "%*s", MEM_STAT_PRINT_LEN,
				    mem_stat_name(mst, i));
		ret += printed;
		buf += printed;
		len -= printed;
	}
	return ret;
}

int hpp_color_scnprintf(struct perf_hpp *hpp, const char *fmt, ...)
{
	va_list args;
	ssize_t ssize = hpp->size;
	double percent;
	int ret, len;

	va_start(args, fmt);
	len = va_arg(args, int);
	percent = va_arg(args, double);
	ret = percent_color_len_snprintf(hpp->buf, hpp->size, fmt, len, percent);
	va_end(args);

	return (ret >= ssize) ? (ssize - 1) : ret;
}

static int hpp_entry_scnprintf(struct perf_hpp *hpp, const char *fmt, ...)
{
	va_list args;
	ssize_t ssize = hpp->size;
	int ret;

	va_start(args, fmt);
	ret = vsnprintf(hpp->buf, hpp->size, fmt, args);
	va_end(args);

	return (ret >= ssize) ? (ssize - 1) : ret;
}

#define __HPP_COLOR_PERCENT_FN(_type, _field, _fmttype)				\
static u64 he_get_##_field(struct hist_entry *he)				\
{										\
	return he->stat._field;							\
}										\
										\
static int hpp__color_##_type(struct perf_hpp_fmt *fmt,				\
			      struct perf_hpp *hpp, struct hist_entry *he) 	\
{										\
	return hpp__fmt(fmt, hpp, he, he_get_##_field, " %*.2f%%",		\
			hpp_color_scnprintf, _fmttype);				\
}

#define __HPP_ENTRY_PERCENT_FN(_type, _field, _fmttype)				\
static int hpp__entry_##_type(struct perf_hpp_fmt *fmt,				\
			      struct perf_hpp *hpp, struct hist_entry *he) 	\
{										\
	return hpp__fmt(fmt, hpp, he, he_get_##_field, " %*.2f%%",		\
			hpp_entry_scnprintf, _fmttype);	\
}

#define __HPP_SORT_FN(_type, _field)						\
static int64_t hpp__sort_##_type(struct perf_hpp_fmt *fmt __maybe_unused, 	\
				 struct hist_entry *a, struct hist_entry *b) 	\
{										\
	return __hpp__sort(a, b, he_get_##_field);				\
}

#define __HPP_COLOR_ACC_PERCENT_FN(_type, _field, _fmttype)			\
static u64 he_get_acc_##_field(struct hist_entry *he)				\
{										\
	return he->stat_acc->_field;						\
}										\
										\
static int hpp__color_##_type(struct perf_hpp_fmt *fmt,				\
			      struct perf_hpp *hpp, struct hist_entry *he) 	\
{										\
	return hpp__fmt_acc(fmt, hpp, he, he_get_acc_##_field, " %*.2f%%", 	\
			    hpp_color_scnprintf, _fmttype);			\
}

#define __HPP_ENTRY_ACC_PERCENT_FN(_type, _field, _fmttype)			\
static int hpp__entry_##_type(struct perf_hpp_fmt *fmt,				\
			      struct perf_hpp *hpp, struct hist_entry *he) 	\
{										\
	return hpp__fmt_acc(fmt, hpp, he, he_get_acc_##_field, " %*.2f%%",	\
			    hpp_entry_scnprintf, _fmttype);			\
}

#define __HPP_SORT_ACC_FN(_type, _field)					\
static int64_t hpp__sort_##_type(struct perf_hpp_fmt *fmt __maybe_unused, 	\
				 struct hist_entry *a, struct hist_entry *b) 	\
{										\
	return __hpp__sort_acc(a, b, he_get_acc_##_field);			\
}

#define __HPP_ENTRY_RAW_FN(_type, _field)					\
static u64 he_get_raw_##_field(struct hist_entry *he)				\
{										\
	return he->stat._field;							\
}										\
										\
static int hpp__entry_##_type(struct perf_hpp_fmt *fmt,				\
			      struct perf_hpp *hpp, struct hist_entry *he) 	\
{										\
	return hpp__fmt(fmt, hpp, he, he_get_raw_##_field, " %*"PRIu64, 	\
			hpp_entry_scnprintf, PERF_HPP_FMT_TYPE__RAW);		\
}

#define __HPP_SORT_RAW_FN(_type, _field)					\
static int64_t hpp__sort_##_type(struct perf_hpp_fmt *fmt __maybe_unused, 	\
				 struct hist_entry *a, struct hist_entry *b) 	\
{										\
	return __hpp__sort(a, b, he_get_raw_##_field);				\
}

#define __HPP_ENTRY_AVERAGE_FN(_type, _field)					\
static u64 he_get_##_field(struct hist_entry *he)				\
{										\
	return he->stat._field;							\
}										\
										\
static int hpp__entry_##_type(struct perf_hpp_fmt *fmt,				\
			      struct perf_hpp *hpp, struct hist_entry *he) 	\
{										\
	return hpp__fmt(fmt, hpp, he, he_get_##_field, " %*.1f",		\
			hpp_entry_scnprintf, PERF_HPP_FMT_TYPE__AVERAGE);	\
}

#define __HPP_SORT_AVERAGE_FN(_type, _field)					\
static int64_t hpp__sort_##_type(struct perf_hpp_fmt *fmt __maybe_unused, 	\
				 struct hist_entry *a, struct hist_entry *b) 	\
{										\
	return __hpp__sort(a, b, he_get_##_field);				\
}

#define __HPP_COLOR_MEM_STAT_FN(_name, _type)					\
static int hpp__color_mem_stat_##_name(struct perf_hpp_fmt *fmt,		\
				       struct perf_hpp *hpp,			\
				       struct hist_entry *he)			\
{										\
	return hpp__fmt_mem_stat(fmt, hpp, he, PERF_MEM_STAT_##_type,		\
				 " %5.1f%%", hpp_color_scnprintf);		\
}

#define __HPP_ENTRY_MEM_STAT_FN(_name, _type)					\
static int hpp__entry_mem_stat_##_name(struct perf_hpp_fmt *fmt, 		\
				       struct perf_hpp *hpp,			\
				       struct hist_entry *he)			\
{										\
	return hpp__fmt_mem_stat(fmt, hpp, he, PERF_MEM_STAT_##_type,		\
				 " %5.1f%%", hpp_entry_scnprintf);		\
}

#define HPP_PERCENT_FNS(_type, _field, _fmttype)			\
__HPP_COLOR_PERCENT_FN(_type, _field, _fmttype)				\
__HPP_ENTRY_PERCENT_FN(_type, _field, _fmttype)				\
__HPP_SORT_FN(_type, _field)

#define HPP_PERCENT_ACC_FNS(_type, _field, _fmttype)			\
__HPP_COLOR_ACC_PERCENT_FN(_type, _field, _fmttype)			\
__HPP_ENTRY_ACC_PERCENT_FN(_type, _field, _fmttype)			\
__HPP_SORT_ACC_FN(_type, _field)

#define HPP_RAW_FNS(_type, _field)					\
__HPP_ENTRY_RAW_FN(_type, _field)					\
__HPP_SORT_RAW_FN(_type, _field)

#define HPP_AVERAGE_FNS(_type, _field)					\
__HPP_ENTRY_AVERAGE_FN(_type, _field)					\
__HPP_SORT_AVERAGE_FN(_type, _field)

#define HPP_MEM_STAT_FNS(_name, _type)					\
__HPP_COLOR_MEM_STAT_FN(_name, _type)					\
__HPP_ENTRY_MEM_STAT_FN(_name, _type)

HPP_PERCENT_FNS(overhead, period, PERF_HPP_FMT_TYPE__PERCENT)
HPP_PERCENT_FNS(latency, latency, PERF_HPP_FMT_TYPE__LATENCY)
HPP_PERCENT_FNS(overhead_sys, period_sys, PERF_HPP_FMT_TYPE__PERCENT)
HPP_PERCENT_FNS(overhead_us, period_us, PERF_HPP_FMT_TYPE__PERCENT)
HPP_PERCENT_FNS(overhead_guest_sys, period_guest_sys, PERF_HPP_FMT_TYPE__PERCENT)
HPP_PERCENT_FNS(overhead_guest_us, period_guest_us, PERF_HPP_FMT_TYPE__PERCENT)
HPP_PERCENT_ACC_FNS(overhead_acc, period, PERF_HPP_FMT_TYPE__PERCENT)
HPP_PERCENT_ACC_FNS(latency_acc, latency, PERF_HPP_FMT_TYPE__LATENCY)

HPP_RAW_FNS(samples, nr_events)
HPP_RAW_FNS(period, period)

HPP_AVERAGE_FNS(weight1, weight1)
HPP_AVERAGE_FNS(weight2, weight2)
HPP_AVERAGE_FNS(weight3, weight3)

HPP_MEM_STAT_FNS(op, OP)
HPP_MEM_STAT_FNS(cache, CACHE)
HPP_MEM_STAT_FNS(memory, MEMORY)
HPP_MEM_STAT_FNS(snoop, SNOOP)
HPP_MEM_STAT_FNS(dtlb, DTLB)

static int64_t hpp__nop_cmp(struct perf_hpp_fmt *fmt __maybe_unused,
			    struct hist_entry *a __maybe_unused,
			    struct hist_entry *b __maybe_unused)
{
	return 0;
}

static bool perf_hpp__is_mem_stat_entry(struct perf_hpp_fmt *fmt)
{
	return fmt->sort == hpp__sort_mem_stat;
}

static bool perf_hpp__is_hpp_entry(struct perf_hpp_fmt *a)
{
	return a->header == hpp__header_fn;
}

static bool hpp__equal(struct perf_hpp_fmt *a, struct perf_hpp_fmt *b)
{
	if (!perf_hpp__is_hpp_entry(a) || !perf_hpp__is_hpp_entry(b))
		return false;

	return a->idx == b->idx;
}

static bool hpp__equal_mem_stat(struct perf_hpp_fmt *a, struct perf_hpp_fmt *b)
{
	if (!perf_hpp__is_mem_stat_entry(a) || !perf_hpp__is_mem_stat_entry(b))
		return false;

	return a->entry == b->entry;
}

#define HPP__COLOR_PRINT_FNS(_name, _fn, _idx)		\
	{						\
		.name   = _name,			\
		.header	= hpp__header_fn,		\
		.width	= hpp__width_fn,		\
		.color	= hpp__color_ ## _fn,		\
		.entry	= hpp__entry_ ## _fn,		\
		.cmp	= hpp__nop_cmp,			\
		.collapse = hpp__nop_cmp,		\
		.sort	= hpp__sort_ ## _fn,		\
		.idx	= PERF_HPP__ ## _idx,		\
		.equal	= hpp__equal,			\
	}

#define HPP__COLOR_ACC_PRINT_FNS(_name, _fn, _idx)	\
	{						\
		.name   = _name,			\
		.header	= hpp__header_fn,		\
		.width	= hpp__width_fn,		\
		.color	= hpp__color_ ## _fn,		\
		.entry	= hpp__entry_ ## _fn,		\
		.cmp	= hpp__nop_cmp,			\
		.collapse = hpp__nop_cmp,		\
		.sort	= hpp__sort_ ## _fn,		\
		.idx	= PERF_HPP__ ## _idx,		\
		.equal	= hpp__equal,			\
	}

#define HPP__PRINT_FNS(_name, _fn, _idx)		\
	{						\
		.name   = _name,			\
		.header	= hpp__header_fn,		\
		.width	= hpp__width_fn,		\
		.entry	= hpp__entry_ ## _fn,		\
		.cmp	= hpp__nop_cmp,			\
		.collapse = hpp__nop_cmp,		\
		.sort	= hpp__sort_ ## _fn,		\
		.idx	= PERF_HPP__ ## _idx,		\
		.equal	= hpp__equal,			\
	}

#define HPP__MEM_STAT_PRINT_FNS(_name, _fn, _type)	\
	{						\
		.name   = _name,			\
		.header	= hpp__header_mem_stat_fn,	\
		.width	= hpp__width_fn,		\
		.color	= hpp__color_mem_stat_ ## _fn,	\
		.entry	= hpp__entry_mem_stat_ ## _fn,	\
		.cmp	= hpp__nop_cmp,			\
		.collapse = hpp__nop_cmp,		\
		.sort	= hpp__sort_mem_stat,		\
		.idx	= PERF_HPP__MEM_STAT_ ## _type,	\
		.equal	= hpp__equal_mem_stat,		\
	}

struct perf_hpp_fmt perf_hpp__format[] = {
	HPP__COLOR_PRINT_FNS("Overhead", overhead, OVERHEAD),
	HPP__COLOR_PRINT_FNS("Latency", latency, LATENCY),
	HPP__COLOR_PRINT_FNS("sys", overhead_sys, OVERHEAD_SYS),
	HPP__COLOR_PRINT_FNS("usr", overhead_us, OVERHEAD_US),
	HPP__COLOR_PRINT_FNS("guest sys", overhead_guest_sys, OVERHEAD_GUEST_SYS),
	HPP__COLOR_PRINT_FNS("guest usr", overhead_guest_us, OVERHEAD_GUEST_US),
	HPP__COLOR_ACC_PRINT_FNS("Children", overhead_acc, OVERHEAD_ACC),
	HPP__COLOR_ACC_PRINT_FNS("Latency", latency_acc, LATENCY_ACC),
	HPP__PRINT_FNS("Samples", samples, SAMPLES),
	HPP__PRINT_FNS("Period", period, PERIOD),
	HPP__PRINT_FNS("Weight1", weight1, WEIGHT1),
	HPP__PRINT_FNS("Weight2", weight2, WEIGHT2),
	HPP__PRINT_FNS("Weight3", weight3, WEIGHT3),
	HPP__MEM_STAT_PRINT_FNS("Mem Op", op, OP),
	HPP__MEM_STAT_PRINT_FNS("Cache", cache, CACHE),
	HPP__MEM_STAT_PRINT_FNS("Memory", memory, MEMORY),
	HPP__MEM_STAT_PRINT_FNS("Snoop", snoop, SNOOP),
	HPP__MEM_STAT_PRINT_FNS("D-TLB", dtlb, DTLB),
};

struct perf_hpp_list perf_hpp_list = {
	.fields	= LIST_HEAD_INIT(perf_hpp_list.fields),
	.sorts	= LIST_HEAD_INIT(perf_hpp_list.sorts),
	.nr_header_lines = 1,
};

#undef HPP__COLOR_PRINT_FNS
#undef HPP__COLOR_ACC_PRINT_FNS
#undef HPP__PRINT_FNS
#undef HPP__MEM_STAT_PRINT_FNS

#undef HPP_PERCENT_FNS
#undef HPP_PERCENT_ACC_FNS
#undef HPP_RAW_FNS
#undef HPP_AVERAGE_FNS
#undef HPP_MEM_STAT_FNS

#undef __HPP_HEADER_FN
#undef __HPP_WIDTH_FN
#undef __HPP_COLOR_PERCENT_FN
#undef __HPP_ENTRY_PERCENT_FN
#undef __HPP_COLOR_ACC_PERCENT_FN
#undef __HPP_ENTRY_ACC_PERCENT_FN
#undef __HPP_ENTRY_RAW_FN
#undef __HPP_ENTRY_AVERAGE_FN
#undef __HPP_COLOR_MEM_STAT_FN
#undef __HPP_ENTRY_MEM_STAT_FN

#undef __HPP_SORT_FN
#undef __HPP_SORT_ACC_FN
#undef __HPP_SORT_RAW_FN
#undef __HPP_SORT_AVERAGE_FN

static void fmt_free(struct perf_hpp_fmt *fmt)
{
	/*
	 * At this point fmt should be completely
	 * unhooked, if not it's a bug.
	 */
	BUG_ON(!list_empty(&fmt->list));
	BUG_ON(!list_empty(&fmt->sort_list));

	if (fmt->free)
		fmt->free(fmt);
}

static bool fmt_equal(struct perf_hpp_fmt *a, struct perf_hpp_fmt *b)
{
	return a->equal && a->equal(a, b);
}

void perf_hpp__init(void)
{
	int i;

	for (i = 0; i < PERF_HPP__MAX_INDEX; i++) {
		struct perf_hpp_fmt *fmt = &perf_hpp__format[i];

		INIT_LIST_HEAD(&fmt->list);

		/* sort_list may be linked by setup_sorting() */
		if (fmt->sort_list.next == NULL)
			INIT_LIST_HEAD(&fmt->sort_list);
	}

	/*
	 * If user specified field order, no need to setup default fields.
	 */
	if (is_strict_order(field_order))
		return;

	/*
	 * Overhead and latency columns are added in setup_overhead(),
	 * so they are added implicitly here only if they were added
	 * by setup_overhead() before (have was_taken flag set).
	 * This is required because setup_overhead() has more complex
	 * logic, in particular it does not add "overhead" if user
	 * specified "latency" in sort order, and vise versa.
	 */
	if (symbol_conf.cumulate_callchain) {
		/*
		 * Addition of fields is idempotent, so we add latency
		 * column twice to get desired order with simpler logic.
		 */
		if (symbol_conf.prefer_latency)
			hpp_dimension__add_output(PERF_HPP__LATENCY_ACC, true);
		hpp_dimension__add_output(PERF_HPP__OVERHEAD_ACC, true);
		if (symbol_conf.enable_latency)
			hpp_dimension__add_output(PERF_HPP__LATENCY_ACC, true);
		perf_hpp__format[PERF_HPP__OVERHEAD].name = "Self";
	}

	if (symbol_conf.prefer_latency)
		hpp_dimension__add_output(PERF_HPP__LATENCY, true);
	hpp_dimension__add_output(PERF_HPP__OVERHEAD, true);
	if (symbol_conf.enable_latency)
		hpp_dimension__add_output(PERF_HPP__LATENCY, true);

	if (symbol_conf.show_cpu_utilization) {
		hpp_dimension__add_output(PERF_HPP__OVERHEAD_SYS, false);
		hpp_dimension__add_output(PERF_HPP__OVERHEAD_US, false);

		if (perf_guest) {
			hpp_dimension__add_output(PERF_HPP__OVERHEAD_GUEST_SYS, false);
			hpp_dimension__add_output(PERF_HPP__OVERHEAD_GUEST_US, false);
		}
	}

	if (symbol_conf.show_nr_samples)
		hpp_dimension__add_output(PERF_HPP__SAMPLES, false);

	if (symbol_conf.show_total_period)
		hpp_dimension__add_output(PERF_HPP__PERIOD, false);
}

void perf_hpp_list__column_register(struct perf_hpp_list *list,
				    struct perf_hpp_fmt *format)
{
	list_add_tail(&format->list, &list->fields);
}

void perf_hpp_list__register_sort_field(struct perf_hpp_list *list,
					struct perf_hpp_fmt *format)
{
	list_add_tail(&format->sort_list, &list->sorts);
}

void perf_hpp_list__prepend_sort_field(struct perf_hpp_list *list,
				       struct perf_hpp_fmt *format)
{
	list_add(&format->sort_list, &list->sorts);
}

static void perf_hpp__column_unregister(struct perf_hpp_fmt *format)
{
	list_del_init(&format->list);
	list_del_init(&format->sort_list);
	fmt_free(format);
}

void perf_hpp__cancel_cumulate(struct evlist *evlist)
{
	struct perf_hpp_fmt *fmt, *acc, *ovh, *acc_lat, *tmp;
	struct evsel *evsel;

	if (is_strict_order(field_order))
		return;

	ovh = &perf_hpp__format[PERF_HPP__OVERHEAD];
	acc = &perf_hpp__format[PERF_HPP__OVERHEAD_ACC];
	acc_lat = &perf_hpp__format[PERF_HPP__LATENCY_ACC];

	perf_hpp_list__for_each_format_safe(&perf_hpp_list, fmt, tmp) {
		if (fmt_equal(acc, fmt) || fmt_equal(acc_lat, fmt)) {
			perf_hpp__column_unregister(fmt);
			continue;
		}

		if (fmt_equal(ovh, fmt))
			fmt->name = "Overhead";
	}

	evlist__for_each_entry(evlist, evsel) {
		struct hists *hists = evsel__hists(evsel);
		struct perf_hpp_list_node *node;

		list_for_each_entry(node, &hists->hpp_formats, list) {
			perf_hpp_list__for_each_format_safe(&node->hpp, fmt, tmp) {
				if (fmt_equal(acc, fmt) || fmt_equal(acc_lat, fmt)) {
					perf_hpp__column_unregister(fmt);
					continue;
				}

				if (fmt_equal(ovh, fmt))
					fmt->name = "Overhead";
			}
		}
	}
}

void perf_hpp__cancel_latency(struct evlist *evlist)
{
	struct perf_hpp_fmt *fmt, *lat, *acc, *tmp;
	struct evsel *evsel;

	if (is_strict_order(field_order))
		return;
	if (sort_order && strstr(sort_order, "latency"))
		return;

	lat = &perf_hpp__format[PERF_HPP__LATENCY];
	acc = &perf_hpp__format[PERF_HPP__LATENCY_ACC];

	perf_hpp_list__for_each_format_safe(&perf_hpp_list, fmt, tmp) {
		if (fmt_equal(lat, fmt) || fmt_equal(acc, fmt))
			perf_hpp__column_unregister(fmt);
	}

	evlist__for_each_entry(evlist, evsel) {
		struct hists *hists = evsel__hists(evsel);
		struct perf_hpp_list_node *node;

		list_for_each_entry(node, &hists->hpp_formats, list) {
			perf_hpp_list__for_each_format_safe(&node->hpp, fmt, tmp) {
				if (fmt_equal(lat, fmt) || fmt_equal(acc, fmt))
					perf_hpp__column_unregister(fmt);
			}
		}
	}
}

void perf_hpp__setup_output_field(struct perf_hpp_list *list)
{
	struct perf_hpp_fmt *fmt;

	/* append sort keys to output field */
	perf_hpp_list__for_each_sort_list(list, fmt) {
		struct perf_hpp_fmt *pos;

		/* skip sort-only fields ("sort_compute" in perf diff) */
		if (!fmt->entry && !fmt->color)
			continue;

		perf_hpp_list__for_each_format(list, pos) {
			if (fmt_equal(fmt, pos))
				goto next;
		}

		perf_hpp__column_register(fmt);
next:
		continue;
	}
}

void perf_hpp__append_sort_keys(struct perf_hpp_list *list)
{
	struct perf_hpp_fmt *fmt;

	/* append output fields to sort keys */
	perf_hpp_list__for_each_format(list, fmt) {
		struct perf_hpp_fmt *pos;

		perf_hpp_list__for_each_sort_list(list, pos) {
			if (fmt_equal(fmt, pos))
				goto next;
		}

		perf_hpp__register_sort_field(fmt);
next:
		continue;
	}
}


void perf_hpp__reset_output_field(struct perf_hpp_list *list)
{
	struct perf_hpp_fmt *fmt, *tmp;

	/* reset output fields */
	perf_hpp_list__for_each_format_safe(list, fmt, tmp)
		perf_hpp__column_unregister(fmt);

	/* reset sort keys */
	perf_hpp_list__for_each_sort_list_safe(list, fmt, tmp)
		perf_hpp__column_unregister(fmt);
}

/*
 * See hists__fprintf to match the column widths
 */
unsigned int hists__sort_list_width(struct hists *hists)
{
	struct perf_hpp_fmt *fmt;
	int ret = 0;
	bool first = true;
	struct perf_hpp dummy_hpp;

	hists__for_each_format(hists, fmt) {
		if (perf_hpp__should_skip(fmt, hists))
			continue;

		if (first)
			first = false;
		else
			ret += 2;

		ret += fmt->width(fmt, &dummy_hpp, hists);
	}

	if (verbose > 0 && hists__has(hists, sym)) /* Addr + origin */
		ret += 3 + BITS_PER_LONG / 4;

	return ret;
}

unsigned int hists__overhead_width(struct hists *hists)
{
	struct perf_hpp_fmt *fmt;
	int ret = 0;
	bool first = true;
	struct perf_hpp dummy_hpp;

	hists__for_each_format(hists, fmt) {
		if (perf_hpp__is_sort_entry(fmt) || perf_hpp__is_dynamic_entry(fmt))
			break;

		if (first)
			first = false;
		else
			ret += 2;

		ret += fmt->width(fmt, &dummy_hpp, hists);
	}

	return ret;
}

void perf_hpp__reset_width(struct perf_hpp_fmt *fmt, struct hists *hists)
{
	if (perf_hpp__is_sort_entry(fmt))
		return perf_hpp__reset_sort_width(fmt, hists);

	if (perf_hpp__is_dynamic_entry(fmt))
		return;

	BUG_ON(fmt->idx >= PERF_HPP__MAX_INDEX);

	switch (fmt->idx) {
	case PERF_HPP__OVERHEAD:
	case PERF_HPP__LATENCY:
	case PERF_HPP__OVERHEAD_SYS:
	case PERF_HPP__OVERHEAD_US:
	case PERF_HPP__OVERHEAD_ACC:
		fmt->len = 8;
		break;

	case PERF_HPP__OVERHEAD_GUEST_SYS:
	case PERF_HPP__OVERHEAD_GUEST_US:
		fmt->len = 9;
		break;

	case PERF_HPP__SAMPLES:
	case PERF_HPP__PERIOD:
		fmt->len = 12;
		break;

	case PERF_HPP__WEIGHT1:
	case PERF_HPP__WEIGHT2:
	case PERF_HPP__WEIGHT3:
		fmt->len = 8;
		break;

	case PERF_HPP__MEM_STAT_OP:
	case PERF_HPP__MEM_STAT_CACHE:
	case PERF_HPP__MEM_STAT_MEMORY:
	case PERF_HPP__MEM_STAT_SNOOP:
	case PERF_HPP__MEM_STAT_DTLB:
		fmt->len = MEM_STAT_LEN * MEM_STAT_PRINT_LEN;
		break;

	default:
		break;
	}
}

void hists__reset_column_width(struct hists *hists)
{
	struct perf_hpp_fmt *fmt;
	struct perf_hpp_list_node *node;

	hists__for_each_format(hists, fmt)
		perf_hpp__reset_width(fmt, hists);

	/* hierarchy entries have their own hpp list */
	list_for_each_entry(node, &hists->hpp_formats, list) {
		perf_hpp_list__for_each_format(&node->hpp, fmt)
			perf_hpp__reset_width(fmt, hists);
	}
}

void perf_hpp__set_user_width(const char *width_list_str)
{
	struct perf_hpp_fmt *fmt;
	const char *ptr = width_list_str;

	perf_hpp_list__for_each_format(&perf_hpp_list, fmt) {
		char *p;

		int len = strtol(ptr, &p, 10);
		fmt->user_len = len;

		if (*p == ',')
			ptr = p + 1;
		else
			break;
	}
}

static int add_hierarchy_fmt(struct hists *hists, struct perf_hpp_fmt *fmt)
{
	struct perf_hpp_list_node *node = NULL;
	struct perf_hpp_fmt *fmt_copy;
	bool found = false;
	bool skip = perf_hpp__should_skip(fmt, hists);

	list_for_each_entry(node, &hists->hpp_formats, list) {
		if (node->level == fmt->level) {
			found = true;
			break;
		}
	}

	if (!found) {
		node = malloc(sizeof(*node));
		if (node == NULL)
			return -1;

		node->skip = skip;
		node->level = fmt->level;
		perf_hpp_list__init(&node->hpp);

		hists->nr_hpp_node++;
		list_add_tail(&node->list, &hists->hpp_formats);
	}

	fmt_copy = perf_hpp_fmt__dup(fmt);
	if (fmt_copy == NULL)
		return -1;

	if (!skip)
		node->skip = false;

	list_add_tail(&fmt_copy->list, &node->hpp.fields);
	list_add_tail(&fmt_copy->sort_list, &node->hpp.sorts);

	return 0;
}

int perf_hpp__setup_hists_formats(struct perf_hpp_list *list,
				  struct evlist *evlist)
{
	struct evsel *evsel;
	struct perf_hpp_fmt *fmt;
	struct hists *hists;
	int ret;

	if (!symbol_conf.report_hierarchy)
		return 0;

	evlist__for_each_entry(evlist, evsel) {
		hists = evsel__hists(evsel);

		perf_hpp_list__for_each_sort_list(list, fmt) {
			if (perf_hpp__is_dynamic_entry(fmt) &&
			    !perf_hpp__defined_dynamic_entry(fmt, hists))
				continue;

			ret = add_hierarchy_fmt(hists, fmt);
			if (ret < 0)
				return ret;
		}
	}

	return 0;
}

int perf_hpp__alloc_mem_stats(struct perf_hpp_list *list, struct evlist *evlist)
{
	struct perf_hpp_fmt *fmt;
	struct evsel *evsel;
	enum mem_stat_type mst[16];
	unsigned nr_mem_stats = 0;

	perf_hpp_list__for_each_format(list, fmt) {
		if (!perf_hpp__is_mem_stat_entry(fmt))
			continue;

		assert(nr_mem_stats < ARRAY_SIZE(mst));
		mst[nr_mem_stats++] = hpp__mem_stat_type(fmt);
	}

	if (nr_mem_stats == 0)
		return 0;

	list->nr_header_lines = 2;

	evlist__for_each_entry(evlist, evsel) {
		struct hists *hists = evsel__hists(evsel);

		hists->mem_stat_types = calloc(nr_mem_stats,
					       sizeof(*hists->mem_stat_types));
		if (hists->mem_stat_types == NULL)
			return -ENOMEM;

		hists->mem_stat_total = calloc(nr_mem_stats,
					       sizeof(*hists->mem_stat_total));
		if (hists->mem_stat_total == NULL)
			return -ENOMEM;

		memcpy(hists->mem_stat_types, mst, nr_mem_stats * sizeof(*mst));
		hists->nr_mem_stats = nr_mem_stats;
	}
	return 0;
}
