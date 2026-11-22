/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _C5_INPUT_CONFIG_H_
#define _C5_INPUT_CONFIG_H_
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/ctype.h>

#define C5_INPUT_MAX_FIELDS 4
#define C5_INPUT_KEEP (~(u32)0)
#define C5_INPUT_MAX_ROWS 255

struct c5_input_row {
	u32 level;
	u32 cpu_freq;
	u32 bimc_freq;
	u32 hmp_boost;
	u32 head_time;
	u32 tail_time;
};

struct c5_input_param {
	u32 cpu_freq;
	u32 bimc_freq;
	u16 time;
	u8 hmp_boost;
};

/* Match the old kernel's decimal sign grammar, not libc scanf.  Signed
 * fields admit '-' but never '+'.  Numeric prefixes and trailing data are
 * deliberately rejected.  Failure never publishes a partial tuple. */
static int c5_input_parse_fields(const char *buf, size_t count,
				unsigned int nr, unsigned int signed_fields,
				u32 *out)
{
	u32 parsed[C5_INPUT_MAX_FIELDS];
	size_t pos = 0;
	unsigned int i;

	if (!buf || !out || !count || count > 4096 || !nr ||
	    nr > C5_INPUT_MAX_FIELDS || (signed_fields >> nr))
		return -EINVAL;
	for (i = 0; i < nr; i++) {
		u32 value = 0, limit = C5_INPUT_KEEP;
		bool negative = false;

		while (pos < count && isspace((unsigned char)buf[pos]))
			pos++;
		if (pos == count)
			return -EINVAL;
		if ((signed_fields & (1U << i)) && buf[pos] == '-') {
			negative = true;
			pos++;
		}
		if (signed_fields & (1U << i))
			limit = negative ? 2147483648U : 2147483647U;
		if (pos == count || buf[pos] < '0' || buf[pos] > '9')
			return -EINVAL;
		for (; pos < count && buf[pos] >= '0' && buf[pos] <= '9'; pos++) {
			u32 digit = buf[pos] - '0';

			if (value > (limit - digit) / 10)
				return -ERANGE;
			value = value * 10 + digit;
		}
		if (pos < count && !isspace((unsigned char)buf[pos]))
			return -EINVAL;
		parsed[i] = negative ? 0U - value : value;
	}
	while (pos < count && isspace((unsigned char)buf[pos]))
		pos++;
	if (pos != count)
		return -EINVAL;
	for (i = 0; i < nr; i++)
		out[i] = parsed[i];
	return 0;
}

static int c5_input_validate_row(const struct c5_input_row *row)
{
	if (!row->level || row->level > C5_INPUT_MAX_ROWS ||
	    row->cpu_freq == C5_INPUT_KEEP || row->hmp_boost > 1 ||
	    row->head_time > 65535 || row->tail_time > 65535)
		return -ERANGE;
	return 0;
}

enum c5_input_row_field {
	C5_ROW_CPU = 1U << 0,
	C5_ROW_BIMC = 1U << 1,
	C5_ROW_HMP = 1U << 2,
	C5_ROW_HEAD = 1U << 3,
	C5_ROW_TAIL = 1U << 4,
};

/* A fully validated replacement is committed once, under the config lock. */
static int c5_input_patch_row(struct c5_input_row *row, unsigned int fields,
			      const struct c5_input_row *values)
{
	struct c5_input_row copy = *row;
	int ret;

	if (fields & ~31U)
		return -EINVAL;
#define C5_PATCH(bit, member) \
	if ((fields & (bit)) && values->member != C5_INPUT_KEEP) \
		copy.member = values->member
	C5_PATCH(C5_ROW_CPU, cpu_freq);
	C5_PATCH(C5_ROW_BIMC, bimc_freq);
	C5_PATCH(C5_ROW_HMP, hmp_boost);
	C5_PATCH(C5_ROW_HEAD, head_time);
	C5_PATCH(C5_ROW_TAIL, tail_time);
#undef C5_PATCH
	ret = c5_input_validate_row(&copy);
	if (!ret)
		*row = copy;
	return ret;
}

/* Preserve actual old head/tail row selection and the zero-head rule. */
static int c5_input_select_params(const struct c5_input_row *rows,
				 unsigned int nr, unsigned int level,
				 struct c5_input_param params[2],
				 int selected[2])
{
	static const unsigned int map[4][2] = {{2, 0}, {2, 3}, {1, 1}, {1, 2}};
	struct c5_input_param result[2] = {{0}, {0}};
	int found[2] = {-1, -1};
	unsigned int i, phase, which;

	if (!nr || nr > C5_INPUT_MAX_ROWS || level > 4)
		return -EINVAL;
	if (!level)
		goto commit;
	which = nr == 1 ? 2 : nr == 2 ? 3 : level - 1;
	for (phase = 0; phase < 2; phase++) {
		unsigned int id = map[which][phase];

		if (!id)
			continue;
		for (i = 0; i < nr; i++)
			if (rows[i].level == id)
				break;
		if (i == nr)
			return -EINVAL;
		if (c5_input_validate_row(&rows[i]))
			return -ERANGE;
		/* The old class head getter also reports zero when head time is0. */
		if (!phase && !rows[i].head_time)
			continue;
		found[phase] = i;
		result[phase].cpu_freq = rows[i].cpu_freq;
		result[phase].bimc_freq = rows[i].bimc_freq;
		result[phase].hmp_boost = rows[i].hmp_boost;
		result[phase].time = phase ? rows[i].tail_time : rows[i].head_time;
	}
commit:
	for (phase = 0; phase < 2; phase++) {
		params[phase] = result[phase];
		if (selected)
			selected[phase] = found[phase];
	}
	return 0;
}
#endif
