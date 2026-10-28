// SPDX-License-Identifier: GPL-2.0
//
// Spreadtrum clock infrastructure
//
// Copyright (C) 2017 Spreadtrum, Inc.
// Author: Chunyan Zhang <chunyan.zhang@spreadtrum.com>

#ifndef _SPRD_CLK_COMMON_H_
#define _SPRD_CLK_COMMON_H_

#include <clk-uclass.h>
#include <vsprintf.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <div64.h>
#include <dm.h>
#include <asm/io.h>
#include <dm/device.h>
#include <dm/device-internal.h>
#include <dm/lists.h>
#include <dm/uclass.h>
#include <dm/uclass-internal.h>
#include <dm/util.h>
#include <linux/bitops.h>
#include <linux/compat.h>
#include <linux/delay.h>
#include <malloc.h>

/*
 * NOTE: we deliberately do *not* include upstream's <linux/clk-provider.h>
 * here. This whole drivers/clk/sprd/ subtree is a self-contained
 * CCF-style ("struct clk_hw"/"struct clk_ops") shim ported from the
 * vendor Unisoc/Aidata 2015.07 U-Boot tree, and it conflicts with
 * upstream's own (differently-shaped, "struct clk"-based) CCF
 * compatibility layer of the same name - e.g. both declare a
 * clk_hw_get_name() with incompatible prototypes. The handful of
 * generic CCF flag bits normally pulled in from there are hard-coded
 * below instead; their values are long-stable, standard Linux CCF
 * constants.
 */
#ifndef GFP_KERNEL
#define GFP_KERNEL	0
#endif

/* generic clk_hw->init->flags bits (see Linux's include/linux/clk.h) */
#define CLK_SET_RATE_GATE	BIT(0)
#define CLK_SET_PARENT_GATE	BIT(1)
#define CLK_SET_RATE_PARENT	BIT(2)
#define CLK_IGNORE_UNUSED	BIT(3)
#define CLK_IS_BASIC		BIT(5)
#define CLK_GET_RATE_NOCACHE	BIT(6)
#define CLK_SET_RATE_NO_REPARENT BIT(7)
#define CLK_GET_ACCURACY_NOCACHE BIT(8)
#define CLK_IS_CRITICAL		BIT(11)

/* clk-gate flags (see Linux's include/linux/clk-provider.h) */
#define CLK_GATE_SET_TO_DISABLE		BIT(0)
#define CLK_GATE_HIWORD_MASK		BIT(1)

/* clk-divider flags (see Linux's include/linux/clk-provider.h) */
#define CLK_DIVIDER_ONE_BASED		BIT(0)
#define CLK_DIVIDER_POWER_OF_TWO	BIT(1)
#define CLK_DIVIDER_ALLOW_ZERO		BIT(2)
#define CLK_DIVIDER_HIWORD_MASK		BIT(3)
#define CLK_DIVIDER_ROUND_CLOSEST	BIT(4)
#define CLK_DIVIDER_READ_ONLY		BIT(5)
#define CLK_DIVIDER_MAX_AT_ZERO		BIT(6)
#define CLK_DIVIDER_BIG_ENDIAN		BIT(7)

/**
 * struct clk_div_table - table of divider values
 *
 * Divider values can be computed from a table. See Linux's
 * include/linux/clk-provider.h for details - this is used only by
 * drivers/clk/sprd/div.c.
 */
struct clk_div_table {
	unsigned int val;
	unsigned int div;
};

static inline bool is_power_of_2(unsigned long n)
{
	return n != 0 && (n & (n - 1)) == 0;
}

#define WARN_ON(cond)							\
	({								\
		int __ret_warn_on = !!(cond);				\
		if (unlikely(__ret_warn_on))				\
			printf("WARNING: %s:%d %s\n", __FILE__, __LINE__, \
			       #cond);					\
		unlikely(__ret_warn_on);				\
	})

#define __ffs(x) generic_ffs(x)

struct device_node;
struct clk;
struct clk_hw;

/**
 * struct clk_rate_request - Structure encoding the clk constraints that
 * a clock user might require.
 *
 * @rate:		Requested clock rate. This field will be adjusted by
 *			clock drivers according to hardware capabilities.
 * @min_rate:		Minimum rate imposed by clk users.
 * @max_rate:		Maximum rate imposed by clk users.
 * @best_parent_rate:	The best parent rate a parent can provide to fulfill the
 *			requested constraints.
 * @best_parent_hw:	The most appropriate parent clock that fulfills the
 *			requested constraints.
 *
 */
struct clk_rate_request {
	unsigned long rate;
	unsigned long min_rate;
	unsigned long max_rate;
	unsigned long best_parent_rate;
	struct clk_hw *best_parent_hw;
};

/**
 * struct clk_ops -  Callback operations for hardware clocks; these are to
 * be provided by the clock implementation, and will be called by drivers
 * through the clk_* api.
 *
 * @prepare:	Prepare the clock for enabling. This must not return until
 *		the clock is fully prepared, and it's safe to call clk_enable.
 *		This callback is intended to allow clock implementations to
 *		do any initialisation that may sleep. Called with
 *		prepare_lock held.
 *
 * @unprepare:	Release the clock from its prepared state. This will typically
 *		undo any work done in the @prepare callback. Called with
 *		prepare_lock held.
 *
 * @is_prepared: Queries the hardware to determine if the clock is prepared.
 *		This function is allowed to sleep. Optional, if this op is not
 *		set then the prepare count will be used.
 *
 * @unprepare_unused: Unprepare the clock atomically.  Only called from
 *		clk_disable_unused for prepare clocks with special needs.
 *		Called with prepare mutex held. This function may sleep.
 *
 * @enable:	Enable the clock atomically. This must not return until the
 *		clock is generating a valid clock signal, usable by consumer
 *		devices. Called with enable_lock held. This function must not
 *		sleep.
 *
 * @disable:	Disable the clock atomically. Called with enable_lock held.
 *		This function must not sleep.
 *
 * @is_enabled:	Queries the hardware to determine if the clock is enabled.
 *		This function must not sleep. Optional, if this op is not
 *		set then the enable count will be used.
 *
 * @disable_unused: Disable the clock atomically.  Only called from
 *		clk_disable_unused for gate clocks with special needs.
 *		Called with enable_lock held.  This function must not
 *		sleep.
 *
 * @recalc_rate	Recalculate the rate of this clock, by querying hardware. The
 *		parent rate is an input parameter.  It is up to the caller to
 *		ensure that the prepare_mutex is held across this call.
 *		Returns the calculated rate.  Optional, but recommended - if
 *		this op is not set then clock rate will be initialized to 0.
 *
 * @round_rate:	Given a target rate as input, returns the closest rate actually
 *		supported by the clock. The parent rate is an input/output
 *		parameter.
 *
 * @set_parent:	Change the input source of this clock; for clocks with multiple
 *		possible parents specify a new parent by passing in the index
 *		as a u8 corresponding to the parent in either the .parent_names
 *		or .parents arrays.  This function in affect translates an
 *		array index into the value programmed into the hardware.
 *		Returns 0 on success, -EERROR otherwise.
 *
 * @get_parent:	Queries the hardware to determine the parent of a clock.  The
 *		return value is a u8 which specifies the index corresponding to
 *		the parent clock.  This index can be applied to either the
 *		.parent_names or .parents arrays.  In short, this function
 *		translates the parent value read from hardware into an array
 *		index.  Currently only called when the clock is initialized by
 *		__clk_init.  This callback is mandatory for clocks with
 *		multiple parents.  It is optional (and unnecessary) for clocks
 *		with 0 or 1 parents.
 *
 * @set_rate:	Change the rate of this clock. The requested rate is specified
 *		by the second argument, which should typically be the return
 *		of .round_rate call.  The third argument gives the parent rate
 *		which is likely helpful for most .set_rate implementation.
 *		Returns 0 on success, -EERROR otherwise.
 *
 * @recalc_accuracy: Recalculate the accuracy of this clock. The clock accuracy
 *		is expressed in ppb (parts per billion). The parent accuracy is
 *		an input parameter.
 *		Returns the calculated accuracy.  Optional - if	this op is not
 *		set then clock accuracy will be initialized to parent accuracy
 *		or 0 (perfect clock) if clock has no parent.
 * The clk_enable/clk_disable and clk_prepare/clk_unprepare pairs allow
 * implementations to split any work between atomic (enable) and sleepable
 * (prepare) contexts.  If enabling a clock requires code that might sleep,
 * this must be done in clk_prepare.  Clock enable code that will never be
 * called in a sleepable context may be implemented in clk_enable.
 *
 * Typically, drivers will call clk_prepare when a clock may be needed later
 * (eg. when a device is opened), and clk_enable when the clock is actually
 * required (eg. from an interrupt). Note that clk_prepare MUST have been
 * called before clk_enable.
 */
struct sprd_clk_ops {
	int		(*prepare)(struct clk_hw *hw);
	int		(*unprepare)(struct clk_hw *hw);
	int		(*is_prepared)(struct clk_hw *hw);
	void		(*unprepare_unused)(struct clk_hw *hw);
	int		(*enable)(struct clk_hw *hw);
	int		(*disable)(struct clk_hw *hw);
	int		(*is_enabled)(struct clk_hw *hw);
	void		(*disable_unused)(struct clk_hw *hw);
	unsigned long	(*recalc_rate)(struct clk_hw *hw,
					unsigned long parent_rate);
	long		(*round_rate)(struct clk_hw *hw, unsigned long rate,
					unsigned long *parent_rate);
	int		(*determine_rate)(struct clk_hw *hw,
					  struct clk_rate_request *req);
	int		(*set_parent)(struct clk_hw *hw, u8 index);
	int		(*get_parent)(struct clk_hw *hw);
	int		(*set_rate)(struct clk_hw *hw, unsigned long rate,
				    unsigned long parent_rate);
	unsigned long	(*recalc_accuracy)(struct clk_hw *hw,
					   unsigned long parent_accuracy);
};

/**
 * struct clk_init_data - holds init data that's common to all clocks and is
 * shared between the clock provider and the common clock framework.
 *
 * @name: clock name
 * @ops: operations this clock supports
 * @parent_names: array of string names for all possible parents
 * @num_parents: number of possible parents
 * @flags: framework-level hints and quirks
 */
struct clk_init_data {
	const char		*name;
	const struct sprd_clk_ops	*ops;
	const char		* const *parent_names;
	u8			num_parents;
	unsigned long		flags;
};

/**
 * struct clk_hw - handle for traversing from a struct clk to its corresponding
 * hardware-specific structure.  struct clk_hw should be declared within struct
 * clk_foo and then referenced by the struct clk instance that uses struct
 * clk_foo's clk_ops
 *
 * @core: pointer to the struct clk_core instance that points back to this
 * struct clk_hw instance
 *
 * @clk: pointer to the per-user struct clk instance that can be used to call
 * into the clk API
 *
 * @init: pointer to struct clk_init_data that contains the init data shared
 * with the common clock framework.
 */
struct clk_hw {
	const struct clk_init_data *init;
};

struct sprd_clk_common {
	void __iomem *reg_base;
	union {
		u32		reg;
		u32		id;
	};
	struct clk_hw	hw;
};

struct clk_hw_onecell_data {
	unsigned int num;
	struct clk_hw *hws[];
};

struct sprd_clk_desc {
	struct sprd_clk_common		**clk_clks;
	unsigned long			num_clk_clks;
	struct clk_hw_onecell_data      *hw_clks;
};
/**
 * struct clk_fixed_rate - fixed-rate clock
 * @hw:		handle between common and hardware-specific interfaces
 * @fixed_rate:	constant frequency of clock
 */
struct sprd_clk_fixed_rate {
	struct		clk_hw hw;
	unsigned long	fixed_rate;
	unsigned long	fixed_accuracy;
	u8		flags;
};

#define to_sprd_clk_fixed_rate(_hw) container_of(_hw, struct sprd_clk_fixed_rate, hw)

struct sprd_fixed_rate_clk_desc {
	struct sprd_clk_fixed_rate	**clk_clks;
	unsigned long	num_clk_clks;
	struct clk_hw_onecell_data	*hw_clks;
};

struct sprd_clk_fixed_factor {
	struct clk_hw	hw;
	unsigned int	mult;
	unsigned int	div;
};

#define to_sprd_clk_fixed_factor(_hw) container_of(_hw, struct sprd_clk_fixed_factor, hw)

extern const struct sprd_clk_ops sprd_clk_fixed_factor_ops;

#define CLK_HW_INIT(_name, _parent, _ops, _flags)		\
	(&(struct clk_init_data) {				\
		.flags		= _flags,			\
		.name		= _name,			\
		.parent_names	= (const char *[]) { _parent },	\
		.num_parents	= 1,				\
		.ops		= _ops,				\
	})

#define CLK_HW_INIT_PARENTS(_name, _parents, _ops, _flags)	\
	(&(struct clk_init_data) {				\
		.flags		= _flags,			\
		.name		= _name,			\
		.parent_names	= _parents,			\
		.num_parents	= ARRAY_SIZE(_parents),		\
		.ops		= _ops,				\
	})

#define CLK_HW_INIT_NO_PARENT(_name, _ops, _flags)	\
	(&(struct clk_init_data) {			\
		.flags          = _flags,		\
		.name           = _name,		\
		.parent_names   = NULL,			\
		.num_parents    = 0,			\
		.ops            = _ops,			\
	})

#define CLK_FIXED_FACTOR(_struct, _name, _parent,			\
			_div, _mult, _flags)				\
	struct sprd_clk_fixed_factor _struct = {			\
		.div		= _div,					\
		.mult		= _mult,				\
		.hw.init	= CLK_HW_INIT(_name,			\
					      _parent,			\
					      &sprd_clk_fixed_factor_ops,\
					      _flags),			\
	}

extern const struct clk_ops clk_uclass_ops;

static inline struct sprd_clk_common *
	hw_to_sprd_clk_common(const struct clk_hw *hw)
{
	return container_of(hw, struct sprd_clk_common, hw);
}
int sprd_clk_regmap_init(struct udevice *dev,
			 const struct sprd_clk_desc *desc);

const char *clk_hw_get_name(const struct clk_hw *hw);
unsigned int clk_hw_get_num_parents(const struct clk_hw *hw);
unsigned long clk_hw_get_flags(const struct clk_hw *hw);
int uclass_find_clk_by_name(enum uclass_id id, const char *name, struct clk *clk);
#endif /* _SPRD_CLK_COMMON_H_ */
