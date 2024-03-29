/* Target description support for GDB.

   Copyright (C) 2006, 2007, 2008, 2009, 2010 Free Software Foundation, Inc.

   Contributed by CodeSourcery.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"
#include "arch-utils.h"
#include "gdbcmd.h"
#include "gdbtypes.h"
#include "observer.h"
#include "reggroups.h"
#include "target.h"
#include "target-descriptions.h"
#include "value.h"
#include "vec.h"
#include "xml-support.h"
#include "xml-tdesc.h"
#include "osabi.h"

#include "gdb_assert.h"
#include "gdb_obstack.h"
#include "hashtab.h"
#include "splay-tree.h"

/* Types.  */

typedef struct property
{
  char *key;
  char *value;
} property_s;
DEF_VEC_O(property_s);

/* An individual register from a target description.  */

typedef struct tdesc_reg
{
  /* The name of this register.  In standard features, it may be
     recognized by the architecture support code, or it may be purely
     for the user.  */
  char *name;

  /* The register number used by this target to refer to this
     register.  This is used for remote p/P packets and to determine
     the ordering of registers in the remote g/G packets.  */
  long target_regnum;

  /* If this flag is set, GDB should save and restore this register
     around calls to an inferior function.  */
  int save_restore;

  /* The name of the register group containing this register, or NULL
     if the group should be automatically determined from the
     register's type.  If this is "general", "float", or "vector", the
     corresponding "info" command should display this register's
     value.  It can be an arbitrary string, but should be limited to
     alphanumeric characters and internal hyphens.  Currently other
     strings are ignored (treated as NULL).  */
  char *group;

  /* The size of the register, in bits.  */
  int bitsize;

  /* The type of the register.  This string corresponds to either
     a named type from the target description or a predefined
     type from GDB.  */
  char *type;

  /* The target-described type corresponding to TYPE, if found.  */
  struct tdesc_type *tdesc_type;
} *tdesc_reg_p;
DEF_VEC_P(tdesc_reg_p);

/* A named type from a target description.  */

typedef struct tdesc_space
{
  /* The name of the GDB variable ('$foo') representing this space.  */
  char *name;

  /* The annex we do qXfer requests on to read and write its members.  */
  char *annex;

  /* The struct type we built to describe this space's layout.  */
  struct tdesc_type *tdesc_type;

  /* The mapping from local structure offsets to remote register
     addresses.  */
  void *offset_map;
} *tdesc_space_p;
DEF_VEC_P(tdesc_space_p);

typedef struct tdesc_type_field
{
  char *name;
  struct tdesc_type *type;
  int read_sensitive;
  int start, end;
} tdesc_type_field;
DEF_VEC_O(tdesc_type_field);

typedef struct tdesc_type_flag
{
  char *name;
  int start;
} tdesc_type_flag;
DEF_VEC_O(tdesc_type_flag);

typedef struct tdesc_type
{
  /* The name of this type.  */
  char *name;

  /* Identify the kind of this type.  */
  enum
  {
    /* Predefined types.  */
    TDESC_TYPE_INT8,
    TDESC_TYPE_INT16,
    TDESC_TYPE_INT32,
    TDESC_TYPE_INT64,
    TDESC_TYPE_INT128,
    TDESC_TYPE_UINT8,
    TDESC_TYPE_UINT16,
    TDESC_TYPE_UINT32,
    TDESC_TYPE_UINT64,
    TDESC_TYPE_UINT128,
    TDESC_TYPE_CODE_PTR,
    TDESC_TYPE_DATA_PTR,
    TDESC_TYPE_IEEE_SINGLE,
    TDESC_TYPE_IEEE_DOUBLE,
    TDESC_TYPE_ARM_FPA_EXT,
    TDESC_TYPE_I387_EXT,

    /* Types defined by a target feature.  */
    TDESC_TYPE_VECTOR,
    TDESC_TYPE_STRUCT,
    TDESC_TYPE_UNION,
    TDESC_TYPE_FLAGS
  } kind;

  /* Kind-specific data.  */
  union
  {
    /* Vector type.  */
    struct
    {
      struct tdesc_type *type;
      int count;
    } v;

    /* Struct or union type.  */
    struct
    {
      VEC(tdesc_type_field) *fields;
      LONGEST size;
    } u;

    /* Flags type.  */
    struct
    {
      VEC(tdesc_type_flag) *flags;
      LONGEST size;
    } f;
  } u;
} *tdesc_type_p;
DEF_VEC_P(tdesc_type_p);

/* A feature from a target description.  Each feature is a collection
   of other elements, e.g. registers and types.  */

typedef struct tdesc_feature
{
  /* The name of this feature.  It may be recognized by the architecture
     support code.  */
  char *name;

  /* The registers associated with this feature.  */
  VEC(tdesc_reg_p) *registers;

  /* The types associated with this feature.  */
  VEC(tdesc_type_p) *types;

  /* The spaces associated with this feature.  */
  VEC(tdesc_space_p) *spaces;
} *tdesc_feature_p;
DEF_VEC_P(tdesc_feature_p);

/* A compatible architecture from a target description.  */
typedef const struct bfd_arch_info *arch_p;
DEF_VEC_P(arch_p);

/* A target description.  */

struct target_desc
{
  /* The architecture reported by the target, if any.  */
  const struct bfd_arch_info *arch;

  /* The osabi reported by the target, if any; GDB_OSABI_UNKNOWN
     otherwise.  */
  enum gdb_osabi osabi;

  /* The list of compatible architectures reported by the target.  */
  VEC(arch_p) *compatible;

  /* Any architecture-specific properties specified by the target.  */
  VEC(property_s) *properties;

  /* The features associated with this target.  */
  VEC(tdesc_feature_p) *features;
};

/* Per-architecture data associated with a target description.  The
   target description may be shared by multiple architectures, but
   this data is private to one gdbarch.  */

typedef struct tdesc_arch_reg
{
  struct tdesc_reg *reg;
  struct type *type;
} tdesc_arch_reg;
DEF_VEC_O(tdesc_arch_reg);

struct tdesc_arch_data
{
  /* A list of register/type pairs, indexed by GDB's internal register number.
     During initialization of the gdbarch this list is used to store
     registers which the architecture assigns a fixed register number.
     Registers which are NULL in this array, or off the end, are
     treated as zero-sized and nameless (i.e. placeholders in the
     numbering).  */
  VEC(tdesc_arch_reg) *arch_regs;

  /* Functions which report the register name, type, and reggroups for
     pseudo-registers.  */
  gdbarch_register_name_ftype *pseudo_register_name;
  gdbarch_register_type_ftype *pseudo_register_type;
  gdbarch_register_reggroup_p_ftype *pseudo_register_reggroup_p;
};

/* Global state.  These variables are associated with the current
   target; if GDB adds support for multiple simultaneous targets, then
   these variables should become target-specific data.  */

/* A flag indicating that a description has already been fetched from
   the current target, so it should not be queried again.  */

static int target_desc_fetched;

/* The description fetched from the current target, or NULL if the
   current target did not supply any description.  Only valid when
   target_desc_fetched is set.  Only the description initialization
   code should access this; normally, the description should be
   accessed through the gdbarch object.  */

static const struct target_desc *current_target_desc;

/* Other global variables.  */

/* The filename to read a target description from.  */

static char *target_description_filename;

/* A handle for architecture-specific data associated with the
   target description (see struct tdesc_arch_data).  */

static struct gdbarch_data *tdesc_data;

/* Prototypes.  */

static void tdesc_free_space (struct tdesc_space *space);

/* Fetch the current target's description, and switch the current
   architecture to one which incorporates that description.  */

void
target_find_description (void)
{
  /* If we've already fetched a description from the target, don't do
     it again.  This allows a target to fetch the description early,
     during its to_open or to_create_inferior, if it needs extra
     information about the target to initialize.  */
  if (target_desc_fetched)
    return;

  /* The current architecture should not have any target description
     specified.  It should have been cleared, e.g. when we
     disconnected from the previous target.  */
  gdb_assert (gdbarch_target_desc (target_gdbarch) == NULL);

  /* First try to fetch an XML description from the user-specified
     file.  */
  current_target_desc = NULL;
  if (target_description_filename != NULL
      && *target_description_filename != '\0')
    current_target_desc
      = file_read_description_xml (target_description_filename);

  /* Next try to read the description from the current target using
     target objects.  */
  if (current_target_desc == NULL)
    current_target_desc = target_read_description_xml (&current_target);

  /* If that failed try a target-specific hook.  */
  if (current_target_desc == NULL)
    current_target_desc = target_read_description (&current_target);

  /* If a non-NULL description was returned, then update the current
     architecture.  */
  if (current_target_desc)
    {
      struct gdbarch_info info;

      gdbarch_info_init (&info);
      info.target_desc = current_target_desc;
      if (!gdbarch_update_p (info))
	warning (_("Architecture rejected target-supplied description"));
      else
	{
	  struct tdesc_arch_data *data;

	  data = gdbarch_data (target_gdbarch, tdesc_data);
	  if (tdesc_has_registers (current_target_desc)
	      && data->arch_regs == NULL)
	    warning (_("Target-supplied registers are not supported "
		       "by the current architecture"));
	}
    }

  /* Now that we know this description is usable, record that we
     fetched it.  */
  target_desc_fetched = 1;
}

/* Discard any description fetched from the current target, and switch
   the current architecture to one with no target description.  */

void
target_clear_description (void)
{
  struct gdbarch_info info;

  if (!target_desc_fetched)
    return;

  target_desc_fetched = 0;
  current_target_desc = NULL;

  gdbarch_info_init (&info);
  if (!gdbarch_update_p (info))
    internal_error (__FILE__, __LINE__,
		    _("Could not remove target-supplied description"));
}

/* Return the global current target description.  This should only be
   used by gdbarch initialization code; most access should be through
   an existing gdbarch.  */

const struct target_desc *
target_current_description (void)
{
  if (target_desc_fetched)
    return current_target_desc;

  return NULL;
}

/* Return non-zero if this target description is compatible
   with the given BFD architecture.  */

int
tdesc_compatible_p (const struct target_desc *target_desc,
		    const struct bfd_arch_info *arch)
{
  const struct bfd_arch_info *compat;
  int ix;

  for (ix = 0; VEC_iterate (arch_p, target_desc->compatible, ix, compat);
       ix++)
    {
      if (compat == arch
	  || arch->compatible (arch, compat)
	  || compat->compatible (compat, arch))
	return 1;
    }

  return 0;
}


/* Direct accessors for target descriptions.  */

/* Return the string value of a property named KEY, or NULL if the
   property was not specified.  */

const char *
tdesc_property (const struct target_desc *target_desc, const char *key)
{
  struct property *prop;
  int ix;

  for (ix = 0; VEC_iterate (property_s, target_desc->properties, ix, prop);
       ix++)
    if (strcmp (prop->key, key) == 0)
      return prop->value;

  return NULL;
}

/* Return the BFD architecture associated with this target
   description, or NULL if no architecture was specified.  */

const struct bfd_arch_info *
tdesc_architecture (const struct target_desc *target_desc)
{
  return target_desc->arch;
}

/* Return the OSABI associated with this target description, or
   GDB_OSABI_UNKNOWN if no osabi was specified.  */

enum gdb_osabi
tdesc_osabi (const struct target_desc *target_desc)
{
  return target_desc->osabi;
}



/* Return 1 if this target description includes any registers.  */

int
tdesc_has_registers (const struct target_desc *target_desc)
{
  int ix;
  struct tdesc_feature *feature;

  if (target_desc == NULL)
    return 0;

  for (ix = 0;
       VEC_iterate (tdesc_feature_p, target_desc->features, ix, feature);
       ix++)
    if (! VEC_empty (tdesc_reg_p, feature->registers))
      return 1;

  return 0;
}

/* Return the feature with the given name, if present, or NULL if
   the named feature is not found.  */

const struct tdesc_feature *
tdesc_find_feature (const struct target_desc *target_desc,
		    const char *name)
{
  int ix;
  struct tdesc_feature *feature;

  for (ix = 0;
       VEC_iterate (tdesc_feature_p, target_desc->features, ix, feature);
       ix++)
    if (strcmp (feature->name, name) == 0)
      return feature;

  return NULL;
}

/* Return the name of FEATURE.  */

const char *
tdesc_feature_name (const struct tdesc_feature *feature)
{
  return feature->name;
}

/* Predefined types.  */
static struct tdesc_type tdesc_predefined_types[] =
{
  { "int8", TDESC_TYPE_INT8 },
  { "int16", TDESC_TYPE_INT16 },
  { "int32", TDESC_TYPE_INT32 },
  { "int64", TDESC_TYPE_INT64 },
  { "int128", TDESC_TYPE_INT128 },
  { "uint8", TDESC_TYPE_UINT8 },
  { "uint16", TDESC_TYPE_UINT16 },
  { "uint32", TDESC_TYPE_UINT32 },
  { "uint64", TDESC_TYPE_UINT64 },
  { "uint128", TDESC_TYPE_UINT128 },
  { "code_ptr", TDESC_TYPE_CODE_PTR },
  { "data_ptr", TDESC_TYPE_DATA_PTR },
  { "ieee_single", TDESC_TYPE_IEEE_SINGLE },
  { "ieee_double", TDESC_TYPE_IEEE_DOUBLE },
  { "arm_fpa_ext", TDESC_TYPE_ARM_FPA_EXT },
  { "i387_ext", TDESC_TYPE_I387_EXT }
};

/* Return the type associated with ID in the context of FEATURE, or
   NULL if none.  */

struct tdesc_type *
tdesc_named_type (const struct tdesc_feature *feature, const char *id)
{
  int ix;
  struct tdesc_type *type;

  /* First try target-defined types.  */
  for (ix = 0; VEC_iterate (tdesc_type_p, feature->types, ix, type); ix++)
    if (strcmp (type->name, id) == 0)
      return type;

  /* Next try the predefined types.  */
  for (ix = 0; ix < ARRAY_SIZE (tdesc_predefined_types); ix++)
    if (strcmp (tdesc_predefined_types[ix].name, id) == 0)
      return &tdesc_predefined_types[ix];

  return NULL;
}

/* Lookup type associated with ID.  */

struct type *
tdesc_find_type (struct gdbarch *gdbarch, const char *id)
{
  struct tdesc_arch_reg *reg;
  struct tdesc_arch_data *data;
  int i, num_regs;

  data = gdbarch_data (gdbarch, tdesc_data);
  num_regs = VEC_length (tdesc_arch_reg, data->arch_regs);
  for (i = 0; i < num_regs; i++)
    {
      reg = VEC_index (tdesc_arch_reg, data->arch_regs, i);
      if (reg->reg
	  && reg->reg->tdesc_type
	  && reg->type
	  && strcmp (id, reg->reg->tdesc_type->name) == 0)
	return reg->type;
    }

  return NULL;
}

/* Construct, if necessary, and return the GDB type implementing target
   type TDESC_TYPE for architecture GDBARCH.  */

static struct type *
tdesc_gdb_type (struct gdbarch *gdbarch, struct tdesc_type *tdesc_type)
{
  struct type *type;

  switch (tdesc_type->kind)
    {
    /* Predefined types.  */
    case TDESC_TYPE_INT8:
      return builtin_type (gdbarch)->builtin_int8;

    case TDESC_TYPE_INT16:
      return builtin_type (gdbarch)->builtin_int16;

    case TDESC_TYPE_INT32:
      return builtin_type (gdbarch)->builtin_int32;

    case TDESC_TYPE_INT64:
      return builtin_type (gdbarch)->builtin_int64;

    case TDESC_TYPE_INT128:
      return builtin_type (gdbarch)->builtin_int128;

    case TDESC_TYPE_UINT8:
      return builtin_type (gdbarch)->builtin_uint8;

    case TDESC_TYPE_UINT16:
      return builtin_type (gdbarch)->builtin_uint16;

    case TDESC_TYPE_UINT32:
      return builtin_type (gdbarch)->builtin_uint32;

    case TDESC_TYPE_UINT64:
      return builtin_type (gdbarch)->builtin_uint64;

    case TDESC_TYPE_UINT128:
      return builtin_type (gdbarch)->builtin_uint128;

    case TDESC_TYPE_CODE_PTR:
      return builtin_type (gdbarch)->builtin_func_ptr;

    case TDESC_TYPE_DATA_PTR:
      return builtin_type (gdbarch)->builtin_data_ptr;

    default:
      break;
    }

  type = tdesc_find_type (gdbarch, tdesc_type->name);
  if (type)
    return type;

  switch (tdesc_type->kind)
    {
    case TDESC_TYPE_IEEE_SINGLE:
      return arch_float_type (gdbarch, -1, "builtin_type_ieee_single",
			      floatformats_ieee_single);

    case TDESC_TYPE_IEEE_DOUBLE:
      return arch_float_type (gdbarch, -1, "builtin_type_ieee_double",
			      floatformats_ieee_double);

    case TDESC_TYPE_ARM_FPA_EXT:
      return arch_float_type (gdbarch, -1, "builtin_type_arm_ext",
			      floatformats_arm_ext);

    case TDESC_TYPE_I387_EXT:
      return arch_float_type (gdbarch, -1, "builtin_type_i387_ext",
			      floatformats_i387_ext);

    /* Types defined by a target feature.  */
    case TDESC_TYPE_VECTOR:
      {
	struct type *type, *field_type;

	field_type = tdesc_gdb_type (gdbarch, tdesc_type->u.v.type);
	type = init_vector_type (field_type, tdesc_type->u.v.count);
	TYPE_NAME (type) = xstrdup (tdesc_type->name);

	return type;
      }

    case TDESC_TYPE_STRUCT:
      {
	struct type *type, *field_type;
	struct tdesc_type_field *f;
	int ix;

	type = arch_composite_type (gdbarch, NULL, TYPE_CODE_STRUCT);
	TYPE_NAME (type) = xstrdup (tdesc_type->name);
	TYPE_TAG_NAME (type) = TYPE_NAME (type);

	for (ix = 0;
	     VEC_iterate (tdesc_type_field, tdesc_type->u.u.fields, ix, f);
	     ix++)
	  {
	    if (f->type == NULL)
	      {
		/* Bitfield.  */
		struct field *fld;
		struct type *field_type;
		int bitsize, total_size;

		/* This invariant should be preserved while creating
		   types.  */
		gdb_assert (tdesc_type->u.u.size != 0);
		if (tdesc_type->u.u.size > 4)
		  field_type = builtin_type (gdbarch)->builtin_uint64;
		else
		  field_type = builtin_type (gdbarch)->builtin_uint32;

		fld = append_composite_type_field_raw (type, xstrdup (f->name),
						       field_type);

		/* For little-endian, BITPOS counts from the LSB of
		   the structure and marks the LSB of the field.  For
		   big-endian, BITPOS counts from the MSB of the
		   structure and marks the MSB of the field.  Either
		   way, it is the number of bits to the "left" of the
		   field.  To calculate this in big-endian, we need
		   the total size of the structure.  */
		bitsize = f->end - f->start + 1;
		total_size = tdesc_type->u.u.size * TARGET_CHAR_BIT;
		if (gdbarch_bits_big_endian (gdbarch))
		  FIELD_BITPOS (fld[0]) = total_size - f->start - bitsize;
		else
		  FIELD_BITPOS (fld[0]) = f->start;
		FIELD_BITSIZE (fld[0]) = bitsize;
	      }
	    else
	      {
		field_type = tdesc_gdb_type (gdbarch, f->type);

		if (f->read_sensitive)
		  field_type
		    = make_qualified_type (field_type,
					   (TYPE_INSTANCE_FLAGS (field_type)
					    | TYPE_INSTANCE_FLAG_READ_SENSITIVE),
					   NULL);

		append_composite_type_field (type, xstrdup (f->name),
					     field_type);
	      }
	  }

	if (tdesc_type->u.u.size != 0)
	  TYPE_LENGTH (type) = tdesc_type->u.u.size;
	return type;
      }

    case TDESC_TYPE_UNION:
      {
	struct type *type, *field_type;
	struct tdesc_type_field *f;
	int ix;

	type = arch_composite_type (gdbarch, NULL, TYPE_CODE_UNION);
	TYPE_NAME (type) = xstrdup (tdesc_type->name);

	for (ix = 0;
	     VEC_iterate (tdesc_type_field, tdesc_type->u.u.fields, ix, f);
	     ix++)
	  {
	    field_type = tdesc_gdb_type (gdbarch, f->type);
	    append_composite_type_field (type, xstrdup (f->name), field_type);

	    /* If any of the children of a union are vectors, flag the
	       union as a vector also.  This allows e.g. a union of two
	       vector types to show up automatically in "info vector".  */
	    if (TYPE_VECTOR (field_type))
	      TYPE_VECTOR (type) = 1;
	  }
	return type;
      }

    case TDESC_TYPE_FLAGS:
      {
	struct tdesc_type_flag *f;
	int ix;

	type = arch_flags_type (gdbarch, xstrdup (tdesc_type->name),
				tdesc_type->u.f.size);
	for (ix = 0;
	     VEC_iterate (tdesc_type_flag, tdesc_type->u.f.flags, ix, f);
	     ix++)
	  /* Note that contrary to the function name, this call will
	     just set the properties of an already-allocated
	     field.  */
	  append_flags_type_flag (type, f->start,
				  *f->name ? f->name : NULL);

	return type;
      }
    }

  internal_error (__FILE__, __LINE__,
		  "Type \"%s\" has an unknown kind %d",
		  tdesc_type->name, tdesc_type->kind);
}


/* Support for registers from target descriptions.  */

/* Construct the per-gdbarch data.  */

static void *
tdesc_data_init (struct obstack *obstack)
{
  struct tdesc_arch_data *data;

  data = OBSTACK_ZALLOC (obstack, struct tdesc_arch_data);
  return data;
}

/* Similar, but for the temporary copy used during architecture
   initialization.  */

struct tdesc_arch_data *
tdesc_data_alloc (void)
{
  return XZALLOC (struct tdesc_arch_data);
}

/* Free something allocated by tdesc_data_alloc, if it is not going
   to be used (for instance if it was unsuitable for the
   architecture).  */

void
tdesc_data_cleanup (void *data_untyped)
{
  struct tdesc_arch_data *data = data_untyped;

  VEC_free (tdesc_arch_reg, data->arch_regs);
  xfree (data);
}

/* Search FEATURE for a register named NAME.  */

static struct tdesc_reg *
tdesc_find_register_early (const struct tdesc_feature *feature,
			   const char *name)
{
  int ixr;
  struct tdesc_reg *reg;

  for (ixr = 0;
       VEC_iterate (tdesc_reg_p, feature->registers, ixr, reg);
       ixr++)
    if (strcasecmp (reg->name, name) == 0)
      return reg;

  return NULL;
}

/* Search FEATURE for a register named NAME.  Assign REGNO to it.  */

int
tdesc_numbered_register (const struct tdesc_feature *feature,
			 struct tdesc_arch_data *data,
			 int regno, const char *name)
{
  struct tdesc_arch_reg arch_reg = { 0 };
  struct tdesc_reg *reg = tdesc_find_register_early (feature, name);

  if (reg == NULL)
    return 0;

  /* Make sure the vector includes a REGNO'th element.  */
  while (regno >= VEC_length (tdesc_arch_reg, data->arch_regs))
    VEC_safe_push (tdesc_arch_reg, data->arch_regs, &arch_reg);

  arch_reg.reg = reg;
  VEC_replace (tdesc_arch_reg, data->arch_regs, regno, &arch_reg);
  return 1;
}

/* Search FEATURE for a register named NAME, but do not assign a fixed
   register number to it.  */

int
tdesc_unnumbered_register (const struct tdesc_feature *feature,
			   const char *name)
{
  struct tdesc_reg *reg = tdesc_find_register_early (feature, name);

  if (reg == NULL)
    return 0;

  return 1;
}

/* Search FEATURE for a register whose name is in NAMES and assign
   REGNO to it.  */

int
tdesc_numbered_register_choices (const struct tdesc_feature *feature,
				 struct tdesc_arch_data *data,
				 int regno, const char *const names[])
{
  int i;

  for (i = 0; names[i] != NULL; i++)
    if (tdesc_numbered_register (feature, data, regno, names[i]))
      return 1;

  return 0;
}

/* Search FEATURE for a register named NAME, and return its size in
   bits.  The register must exist.  */

int
tdesc_register_size (const struct tdesc_feature *feature,
		     const char *name)
{
  struct tdesc_reg *reg = tdesc_find_register_early (feature, name);

  gdb_assert (reg != NULL);
  return reg->bitsize;
}

/* Look up a register by its GDB internal register number.  */

static struct tdesc_arch_reg *
tdesc_find_arch_register (struct gdbarch *gdbarch, int regno)
{
  struct tdesc_arch_data *data;

  data = gdbarch_data (gdbarch, tdesc_data);
  if (regno < VEC_length (tdesc_arch_reg, data->arch_regs))
    return VEC_index (tdesc_arch_reg, data->arch_regs, regno);
  else
    return NULL;
}

static struct tdesc_reg *
tdesc_find_register (struct gdbarch *gdbarch, int regno)
{
  struct tdesc_arch_reg *reg = tdesc_find_arch_register (gdbarch, regno);

  return reg? reg->reg : NULL;
}

/* Return the name of register REGNO, from the target description or
   from an architecture-provided pseudo_register_name method.  */

const char *
tdesc_register_name (struct gdbarch *gdbarch, int regno)
{
  struct tdesc_reg *reg = tdesc_find_register (gdbarch, regno);
  int num_regs = gdbarch_num_regs (gdbarch);
  int num_pseudo_regs = gdbarch_num_pseudo_regs (gdbarch);

  if (reg != NULL)
    return reg->name;

  if (regno >= num_regs && regno < num_regs + num_pseudo_regs)
    {
      struct tdesc_arch_data *data = gdbarch_data (gdbarch, tdesc_data);

      gdb_assert (data->pseudo_register_name != NULL);
      return data->pseudo_register_name (gdbarch, regno);
    }

  return "";
}

struct type *
tdesc_register_type (struct gdbarch *gdbarch, int regno)
{
  struct tdesc_arch_reg *arch_reg = tdesc_find_arch_register (gdbarch, regno);
  struct tdesc_reg *reg = arch_reg? arch_reg->reg : NULL;
  int num_regs = gdbarch_num_regs (gdbarch);
  int num_pseudo_regs = gdbarch_num_pseudo_regs (gdbarch);

  if (reg == NULL && regno >= num_regs && regno < num_regs + num_pseudo_regs)
    {
      struct tdesc_arch_data *data = gdbarch_data (gdbarch, tdesc_data);

      gdb_assert (data->pseudo_register_type != NULL);
      return data->pseudo_register_type (gdbarch, regno);
    }

  if (reg == NULL)
    /* Return "int0_t", since "void" has a misleading size of one.  */
    return builtin_type (gdbarch)->builtin_int0;

  if (arch_reg->type == NULL)
    {
      /* First check for a predefined or target defined type.  */
      if (reg->tdesc_type)
        arch_reg->type = tdesc_gdb_type (gdbarch, reg->tdesc_type);

      /* Next try size-sensitive type shortcuts.  */
      else if (strcmp (reg->type, "float") == 0)
	{
	  if (reg->bitsize == gdbarch_float_bit (gdbarch))
	    arch_reg->type = builtin_type (gdbarch)->builtin_float;
	  else if (reg->bitsize == gdbarch_double_bit (gdbarch))
	    arch_reg->type = builtin_type (gdbarch)->builtin_double;
	  else if (reg->bitsize == gdbarch_long_double_bit (gdbarch))
	    arch_reg->type = builtin_type (gdbarch)->builtin_long_double;
	  else
	    {
	      warning (_("Register \"%s\" has an unsupported size (%d bits)"),
		       reg->name, reg->bitsize);
	      arch_reg->type = builtin_type (gdbarch)->builtin_double;
	    }
	}
      else if (strcmp (reg->type, "int") == 0)
	{
	  if (reg->bitsize == gdbarch_long_bit (gdbarch))
	    arch_reg->type = builtin_type (gdbarch)->builtin_long;
	  else if (reg->bitsize == TARGET_CHAR_BIT)
	    arch_reg->type = builtin_type (gdbarch)->builtin_char;
	  else if (reg->bitsize == gdbarch_short_bit (gdbarch))
	    arch_reg->type = builtin_type (gdbarch)->builtin_short;
	  else if (reg->bitsize == gdbarch_int_bit (gdbarch))
	    arch_reg->type = builtin_type (gdbarch)->builtin_int;
	  else if (reg->bitsize == gdbarch_long_long_bit (gdbarch))
	    arch_reg->type = builtin_type (gdbarch)->builtin_long_long;
	  else if (reg->bitsize == gdbarch_ptr_bit (gdbarch))
	  /* A bit desperate by this point... */
	    arch_reg->type = builtin_type (gdbarch)->builtin_data_ptr;
	  else
	    {
	      warning (_("Register \"%s\" has an unsupported size (%d bits)"),
		       reg->name, reg->bitsize);
	      arch_reg->type = builtin_type (gdbarch)->builtin_long;
	    }
	}

      if (arch_reg->type == NULL)
	internal_error (__FILE__, __LINE__,
			"Register \"%s\" has an unknown type \"%s\"",
			reg->name, reg->type);
    }

  return arch_reg->type;
}

static int
tdesc_remote_register_number (struct gdbarch *gdbarch, int regno)
{
  struct tdesc_reg *reg = tdesc_find_register (gdbarch, regno);

  if (reg != NULL)
    return reg->target_regnum;
  else
    return -1;
}

/* Check whether REGNUM is a member of REGGROUP.  Registers from the
   target description may be classified as general, float, or vector.
   Unlike a gdbarch register_reggroup_p method, this function will
   return -1 if it does not know; the caller should handle registers
   with no specified group.

   Arbitrary strings (other than "general", "float", and "vector")
   from the description are not used; they cause the register to be
   displayed in "info all-registers" but excluded from "info
   registers" et al.  The names of containing features are also not
   used.  This might be extended to display registers in some more
   useful groupings.

   The save-restore flag is also implemented here.  */

int
tdesc_register_in_reggroup_p (struct gdbarch *gdbarch, int regno,
			      struct reggroup *reggroup)
{
  struct tdesc_reg *reg = tdesc_find_register (gdbarch, regno);

  if (reg != NULL && reg->group != NULL)
    {
      int general_p = 0, float_p = 0, vector_p = 0;

      if (strcmp (reg->group, "general") == 0)
	general_p = 1;
      else if (strcmp (reg->group, "float") == 0)
	float_p = 1;
      else if (strcmp (reg->group, "vector") == 0)
	vector_p = 1;

      if (reggroup == float_reggroup)
	return float_p;

      if (reggroup == vector_reggroup)
	return vector_p;

      if (reggroup == general_reggroup)
	return general_p;
    }

  if (reg != NULL
      && (reggroup == save_reggroup || reggroup == restore_reggroup))
    return reg->save_restore;

  return -1;
}

/* Check whether REGNUM is a member of REGGROUP.  Registers with no
   group specified go to the default reggroup function and are handled
   by type.  */

static int
tdesc_register_reggroup_p (struct gdbarch *gdbarch, int regno,
			   struct reggroup *reggroup)
{
  int num_regs = gdbarch_num_regs (gdbarch);
  int num_pseudo_regs = gdbarch_num_pseudo_regs (gdbarch);
  int ret;

  if (regno >= num_regs && regno < num_regs + num_pseudo_regs)
    {
      struct tdesc_arch_data *data = gdbarch_data (gdbarch, tdesc_data);

      if (data->pseudo_register_reggroup_p != NULL)
	return data->pseudo_register_reggroup_p (gdbarch, regno, reggroup);
      /* Otherwise fall through to the default reggroup_p.  */
    }

  ret = tdesc_register_in_reggroup_p (gdbarch, regno, reggroup);
  if (ret != -1)
    return ret;

  return default_register_reggroup_p (gdbarch, regno, reggroup);
}

/* Record architecture-specific functions to call for pseudo-register
   support.  */

void
set_tdesc_pseudo_register_name (struct gdbarch *gdbarch,
				gdbarch_register_name_ftype *pseudo_name)
{
  struct tdesc_arch_data *data = gdbarch_data (gdbarch, tdesc_data);

  data->pseudo_register_name = pseudo_name;
}

void
set_tdesc_pseudo_register_type (struct gdbarch *gdbarch,
				gdbarch_register_type_ftype *pseudo_type)
{
  struct tdesc_arch_data *data = gdbarch_data (gdbarch, tdesc_data);

  data->pseudo_register_type = pseudo_type;
}

void
set_tdesc_pseudo_register_reggroup_p
  (struct gdbarch *gdbarch,
   gdbarch_register_reggroup_p_ftype *pseudo_reggroup_p)
{
  struct tdesc_arch_data *data = gdbarch_data (gdbarch, tdesc_data);

  data->pseudo_register_reggroup_p = pseudo_reggroup_p;
}

/* Update GDBARCH to use the target description for registers.  */

void
tdesc_use_registers (struct gdbarch *gdbarch,
		     const struct target_desc *target_desc,
		     struct tdesc_arch_data *early_data)
{
  int num_regs = gdbarch_num_regs (gdbarch);
  int ixf, ixr;
  struct tdesc_feature *feature;
  struct tdesc_reg *reg;
  struct tdesc_arch_data *data;
  struct tdesc_arch_reg *arch_reg, new_arch_reg = { 0 };
  htab_t reg_hash;

  /* We can't use the description for registers if it doesn't describe
     any.  This function should only be called after validating
     registers, so the caller should know that registers are
     included.  */
  gdb_assert (tdesc_has_registers (target_desc));

  data = gdbarch_data (gdbarch, tdesc_data);
  data->arch_regs = early_data->arch_regs;
  xfree (early_data);

  /* Build up a set of all registers, so that we can assign register
     numbers where needed.  The hash table expands as necessary, so
     the initial size is arbitrary.  */
  reg_hash = htab_create (37, htab_hash_pointer, htab_eq_pointer, NULL);
  for (ixf = 0;
       VEC_iterate (tdesc_feature_p, target_desc->features, ixf, feature);
       ixf++)
    for (ixr = 0;
	 VEC_iterate (tdesc_reg_p, feature->registers, ixr, reg);
	 ixr++)
      {
	void **slot = htab_find_slot (reg_hash, reg, INSERT);

	*slot = reg;
      }

  /* Remove any registers which were assigned numbers by the
     architecture.  */
  for (ixr = 0;
       VEC_iterate (tdesc_arch_reg, data->arch_regs, ixr, arch_reg);
       ixr++)
    if (arch_reg->reg)
      htab_remove_elt (reg_hash, arch_reg->reg);

  /* Assign numbers to the remaining registers and add them to the
     list of registers.  The new numbers are always above gdbarch_num_regs.
     Iterate over the features, not the hash table, so that the order
     matches that in the target description.  */

  gdb_assert (VEC_length (tdesc_arch_reg, data->arch_regs) <= num_regs);
  while (VEC_length (tdesc_arch_reg, data->arch_regs) < num_regs)
    VEC_safe_push (tdesc_arch_reg, data->arch_regs, &new_arch_reg);
  for (ixf = 0;
       VEC_iterate (tdesc_feature_p, target_desc->features, ixf, feature);
       ixf++)
    for (ixr = 0;
	 VEC_iterate (tdesc_reg_p, feature->registers, ixr, reg);
	 ixr++)
      if (htab_find (reg_hash, reg) != NULL)
	{
	  new_arch_reg.reg = reg;
	  VEC_safe_push (tdesc_arch_reg, data->arch_regs, &new_arch_reg);
	  num_regs++;
	}

  htab_delete (reg_hash);

  /* Update the architecture.  */
  set_gdbarch_num_regs (gdbarch, num_regs);
  set_gdbarch_register_name (gdbarch, tdesc_register_name);
  set_gdbarch_register_type (gdbarch, tdesc_register_type);
  set_gdbarch_remote_register_number (gdbarch,
				      tdesc_remote_register_number);
  set_gdbarch_register_reggroup_p (gdbarch, tdesc_register_reggroup_p);
}


/* Methods for constructing a target description.  */

static void
tdesc_free_reg (struct tdesc_reg *reg)
{
  xfree (reg->name);
  xfree (reg->type);
  xfree (reg->group);
  xfree (reg);
}

void
tdesc_create_reg (struct tdesc_feature *feature, const char *name,
		  int regnum, int save_restore, const char *group,
		  int bitsize, const char *type)
{
  struct tdesc_reg *reg = XZALLOC (struct tdesc_reg);

  reg->name = xstrdup (name);
  reg->target_regnum = regnum;
  reg->save_restore = save_restore;
  reg->group = group ? xstrdup (group) : NULL;
  reg->bitsize = bitsize;
  reg->type = type ? xstrdup (type) : xstrdup ("<unknown>");

  /* If the register's type is target-defined, look it up now.  We may not
     have easy access to the containing feature when we want it later.  */
  reg->tdesc_type = tdesc_named_type (feature, reg->type);

  VEC_safe_push (tdesc_reg_p, feature->registers, reg);
}

static void
tdesc_free_type (struct tdesc_type *type)
{
  switch (type->kind)
    {
    case TDESC_TYPE_STRUCT:
    case TDESC_TYPE_UNION:
      {
	struct tdesc_type_field *f;
	int ix;

	for (ix = 0;
	     VEC_iterate (tdesc_type_field, type->u.u.fields, ix, f);
	     ix++)
	  xfree (f->name);

	VEC_free (tdesc_type_field, type->u.u.fields);
      }
      break;

    case TDESC_TYPE_FLAGS:
      {
	struct tdesc_type_flag *f;
	int ix;

	for (ix = 0;
	     VEC_iterate (tdesc_type_flag, type->u.f.flags, ix, f);
	     ix++)
	  xfree (f->name);

	VEC_free (tdesc_type_flag, type->u.f.flags);
      }
      break;

    default:
      break;
    }

  xfree (type->name);
  xfree (type);
}

struct tdesc_type *
tdesc_create_vector (struct tdesc_feature *feature, const char *name,
		     struct tdesc_type *field_type, int count)
{
  struct tdesc_type *type = XZALLOC (struct tdesc_type);

  type->name = xstrdup (name);
  type->kind = TDESC_TYPE_VECTOR;
  type->u.v.type = field_type;
  type->u.v.count = count;

  VEC_safe_push (tdesc_type_p, feature->types, type);
  return type;
}

char *
tdesc_struct_name (struct tdesc_type *type)
{
  gdb_assert (type->kind == TDESC_TYPE_STRUCT);
  return type->name;
}

struct tdesc_type *
tdesc_create_struct (struct tdesc_feature *feature, const char *name)
{
  struct tdesc_type *type = XZALLOC (struct tdesc_type);

  type->name = xstrdup (name);
  type->kind = TDESC_TYPE_STRUCT;

  VEC_safe_push (tdesc_type_p, feature->types, type);
  return type;
}

/* Set the total length of TYPE.  Structs which contain bitfields may
   omit the reserved bits, so the end of the last field may not
   suffice.  */

void
tdesc_set_struct_size (struct tdesc_type *type, LONGEST size)
{
  gdb_assert (type->kind == TDESC_TYPE_STRUCT);
  type->u.u.size = size;
}

struct tdesc_type *
tdesc_create_union (struct tdesc_feature *feature, const char *name)
{
  struct tdesc_type *type = XZALLOC (struct tdesc_type);

  type->name = xstrdup (name);
  type->kind = TDESC_TYPE_UNION;

  VEC_safe_push (tdesc_type_p, feature->types, type);
  return type;
}

struct tdesc_type *
tdesc_create_flags (struct tdesc_feature *feature, const char *name,
		    LONGEST size)
{
  struct tdesc_type *type = XZALLOC (struct tdesc_type);

  type->name = xstrdup (name);
  type->kind = TDESC_TYPE_FLAGS;
  type->u.f.size = size;

  VEC_safe_push (tdesc_type_p, feature->types, type);
  return type;
}

/* Add a new field.  Return a temporary pointer to the field, which
   is only valid until the next call to tdesc_add_field (the vector
   might be reallocated).  */

struct tdesc_type_field *
tdesc_add_field (struct tdesc_type *type, const char *field_name,
		 struct tdesc_type *field_type)
{
  struct tdesc_type_field f = { 0 };

  gdb_assert (type->kind == TDESC_TYPE_UNION
	      || type->kind == TDESC_TYPE_STRUCT);

  f.name = xstrdup (field_name);
  f.type = field_type;

  VEC_safe_push (tdesc_type_field, type->u.u.fields, &f);
  return VEC_last (tdesc_type_field, type->u.u.fields);
}

/* Mark FIELD as read-sensitive.  */

void
tdesc_make_field_sensitive (struct tdesc_type_field *field)
{
  field->read_sensitive = 1;
}
			   
/* Add a new bitfield.  */

void
tdesc_add_bitfield (struct tdesc_type *type, const char *field_name,
		    int start, int end)
{
  struct tdesc_type_field f = { 0 };

  gdb_assert (type->kind == TDESC_TYPE_STRUCT);

  f.name = xstrdup (field_name);
  f.start = start;
  f.end = end;

  VEC_safe_push (tdesc_type_field, type->u.u.fields, &f);
}

void
tdesc_add_flag (struct tdesc_type *type, int start,
		const char *flag_name)
{
  struct tdesc_type_flag f = { 0 };

  gdb_assert (type->kind == TDESC_TYPE_FLAGS);

  f.name = xstrdup (flag_name);
  f.start = start;

  VEC_safe_push (tdesc_type_flag, type->u.f.flags, &f);
}

static void
tdesc_free_feature (struct tdesc_feature *feature)
{
  struct tdesc_reg *reg;
  struct tdesc_space *space;
  struct tdesc_type *type;
  int ix;

  for (ix = 0; VEC_iterate (tdesc_reg_p, feature->registers, ix, reg); ix++)
    tdesc_free_reg (reg);
  VEC_free (tdesc_reg_p, feature->registers);

  for (ix = 0; VEC_iterate (tdesc_space_p, feature->spaces, ix, space); ix++)
    tdesc_free_space (space);
  VEC_free (tdesc_space_p, feature->spaces);

  for (ix = 0; VEC_iterate (tdesc_type_p, feature->types, ix, type); ix++)
    tdesc_free_type (type);
  VEC_free (tdesc_type_p, feature->types);

  xfree (feature->name);
  xfree (feature);
}

struct tdesc_feature *
tdesc_create_feature (struct target_desc *tdesc, const char *name)
{
  struct tdesc_feature *new_feature = XZALLOC (struct tdesc_feature);

  new_feature->name = xstrdup (name);

  VEC_safe_push (tdesc_feature_p, tdesc->features, new_feature);
  return new_feature;
}

struct target_desc *
allocate_target_description (void)
{
  return XZALLOC (struct target_desc);
}

static void
free_target_description (void *arg)
{
  struct target_desc *target_desc = arg;
  struct tdesc_feature *feature;
  struct property *prop;
  int ix;

  for (ix = 0;
       VEC_iterate (tdesc_feature_p, target_desc->features, ix, feature);
       ix++)
    tdesc_free_feature (feature);
  VEC_free (tdesc_feature_p, target_desc->features);

  for (ix = 0;
       VEC_iterate (property_s, target_desc->properties, ix, prop);
       ix++)
    {
      xfree (prop->key);
      xfree (prop->value);
    }
  VEC_free (property_s, target_desc->properties);

  VEC_free (arch_p, target_desc->compatible);

  xfree (target_desc);
}

struct cleanup *
make_cleanup_free_target_description (struct target_desc *target_desc)
{
  return make_cleanup (free_target_description, target_desc);
}

void
tdesc_add_compatible (struct target_desc *target_desc,
		      const struct bfd_arch_info *compatible)
{
  const struct bfd_arch_info *compat;
  int ix;

  /* If this instance of GDB is compiled without BFD support for the
     compatible architecture, simply ignore it -- we would not be able
     to handle it anyway.  */
  if (compatible == NULL)
    return;

  for (ix = 0; VEC_iterate (arch_p, target_desc->compatible, ix, compat);
       ix++)
    if (compat == compatible)
      internal_error (__FILE__, __LINE__,
		      _("Attempted to add duplicate "
			"compatible architecture \"%s\""),
		      compatible->printable_name);

  VEC_safe_push (arch_p, target_desc->compatible, compatible);
}

void
set_tdesc_property (struct target_desc *target_desc,
		    const char *key, const char *value)
{
  struct property *prop, new_prop;
  int ix;

  gdb_assert (key != NULL && value != NULL);

  for (ix = 0; VEC_iterate (property_s, target_desc->properties, ix, prop);
       ix++)
    if (strcmp (prop->key, key) == 0)
      internal_error (__FILE__, __LINE__,
		      _("Attempted to add duplicate property \"%s\""), key);

  new_prop.key = xstrdup (key);
  new_prop.value = xstrdup (value);
  VEC_safe_push (property_s, target_desc->properties, &new_prop);
}

void
set_tdesc_architecture (struct target_desc *target_desc,
			const struct bfd_arch_info *arch)
{
  target_desc->arch = arch;
}

void
set_tdesc_osabi (struct target_desc *target_desc, enum gdb_osabi osabi)
{
  target_desc->osabi = osabi;
}

/* Register space support - currently CodeSourcery local.  */

static void
tdesc_free_space (struct tdesc_space *space)
{
  xfree (space->name);
  xfree (space->annex);
  if (space->offset_map != NULL)
    splay_tree_delete (space->offset_map);
  /* As for target-defined types, we can not free the space's
     type.  */
  
  xfree (space);
}

struct tdesc_space *
tdesc_create_space (struct tdesc_feature *feature, const char *name,
		    const char *annex, struct tdesc_type *tdesc_type)
{
  struct tdesc_space *space = XZALLOC (struct tdesc_space);

  space->name = xstrdup (name);
  space->annex = xstrdup (annex);
  space->tdesc_type = tdesc_type;

  VEC_safe_push (tdesc_space_p, feature->spaces, space);

  return space;
}

/* Record that the bits at offset FROM in SPACE, in the corresponding
   GDB type, should be fetched from offset TO on the target.  */

void
space_record_offset_mapping (struct tdesc_space *space,
			     ULONGEST from, ULONGEST to)
{
  splay_tree_node node;

  if (space->offset_map == NULL)
    space->offset_map = splay_tree_new (splay_tree_compare_ints, NULL, NULL);

  node = splay_tree_lookup (space->offset_map, from);
  if (node != NULL)
    internal_error (__FILE__, __LINE__,
		    "Duplicated entries for 0x%lx in space offset mapping: "
		    "0x%lx and 0x%lx",
		    (long) from, (long) node->value, (long) to);

  node = splay_tree_successor (space->offset_map, from);
  if (node != NULL)
    internal_error (__FILE__, __LINE__,
		    "Tried to add 0x%lx to space offset mapping after 0x%lx",
		    (long) from, (long) node->key);

  /* If two consecutive entries have the same relative offset, then we
     don't need to record the second entry.  This optimization lets us
     coalesce reads from the target.  */
  node = splay_tree_predecessor (space->offset_map, from);
  if (node != NULL && from - node->key == to - node->value)
    return;

  splay_tree_insert (space->offset_map, from, to);
}

/* Return the target offset where we can find bits from offset FROM in
   the GDB type constructed for SPACE.  Set *NEXT_OFFSET to the
   starting GDB offset of the next portion of SPACE, or zero if this
   is the last portion.  The caller may read up to *NEXT_OFFSET - FROM
   bytes from the returned target offset.  */

static ULONGEST
space_map_offset (struct tdesc_space *space, ULONGEST from,
		  ULONGEST *next_offset)
{
  ULONGEST result;
  splay_tree_node node;

  if (space->offset_map == NULL)
    {
      *next_offset = 0;
      return from;
    }

  node = splay_tree_lookup (space->offset_map, from);
  if (node == NULL)
    node = splay_tree_predecessor (space->offset_map, from);

  if (node == NULL)
    internal_error (__FILE__, __LINE__,
		    "No predecessor for %ld in space mapping",
		    (long) from);

  result = node->value + from - node->key;

  node = splay_tree_successor (space->offset_map, from);
  if (node)
    *next_offset = node->key;
  else
    *next_offset = 0;

  return result;
}

/* Space registers.

   If an architecture has a feature that contains spaces, we create an
   internal variable for each space, where reads and writes to that
   register turn into target reads and writes of a given annex.

   For this, the internal variable's value is an lval_computed value,
   with the space as a closure.  */

static void
space_value_read (struct value *v)
{
  struct tdesc_space *space = value_computed_closure (v);
  unsigned length = TYPE_LENGTH (value_type (v));
  LONGEST transferred;
  ULONGEST offset, target_offset;
  gdb_byte *dest = value_contents_all_raw (v);

  /* value_fetch_lazy handles reading bitfields.  */
  gdb_assert (!value_bitsize (v));

  offset = value_offset (v);
  while (length > 0)
    {
      ULONGEST wanted, next_offset;

      /* Map the current offset to a target offset.  */
      target_offset = space_map_offset (space, offset, &next_offset);
      if (next_offset > 0 && next_offset - offset < length)
	wanted = next_offset - offset;
      else
	wanted = length;

      transferred =
	target_read (&current_target, TARGET_OBJECT_SPACES,
		     space->annex, dest, target_offset, wanted);

      if (transferred != wanted)
	error ("Unable to read from register space '%s'", space->name);

      length -= wanted;
      offset += wanted;
      dest += wanted;
    }
}

static void
space_value_write (struct value *v, struct value *fromval)
{
  struct tdesc_space *space = value_computed_closure (v);
  unsigned real_length;
  CORE_ADDR val_offset, offset, real_start;
  LONGEST transferred;
  ULONGEST next_offset;
  gdb_byte buf[16];

  val_offset = value_offset (v);
  if (value_bitsize (v))
    val_offset += value_offset (value_parent (v));
  offset = space_map_offset (space, val_offset, &next_offset);

  real_length = TYPE_LENGTH (value_type (v)) * 8;
  real_start = offset;

  /* For bitfields, adjust to always write a register sized and
     aligned quantity by using the container type of the bitfield.  */
  if (value_bitsize (v))
    {
      unsigned bit_length, adjust;
      LONGEST fieldval;

      bit_length = value_bitsize (v);

      adjust = real_start % (real_length / 8);
      if (adjust != 0)
	{
	  real_start = real_start - adjust;
	  if (real_length < (adjust * 8) + bit_length + value_bitpos (v))
	    error (_("Unable to read multi-word value from register space '%s'"),
		   space->annex);
	}
      if (real_length > 8 * sizeof (buf))
	error (_("Unable to read multi-word value from register space '%s'"),
	       space->annex);

      transferred =
	target_read (&current_target, TARGET_OBJECT_SPACES,
		      space->annex, buf, real_start, real_length / 8);

      if (transferred != real_length / 8)
	error (_("Unable to read from register space '%s'"),
	       space->annex);

      fieldval = value_as_long (fromval);
      modify_field (value_type (v), buf, fieldval,
		    value_bitpos (v) + 8 * adjust,
		    bit_length);
    }
  else
    memcpy (buf, value_contents_all_raw (fromval), real_length / 8);

  transferred =
    target_write (&current_target, TARGET_OBJECT_SPACES,
                  space->annex, buf, real_start, real_length / 8);

  if (transferred != real_length / 8)
    error (_("Unable to write to register space '%s' (annex '%s')"),
           space->name, space->annex);
}

static struct lval_funcs space_value_funcs =
  {
    .read = space_value_read,
    .write = space_value_write,
  };

static struct value *
make_space_value (struct gdbarch *gdbarch, struct tdesc_feature *feature,
		  struct tdesc_space *space)
{
  struct type *type = tdesc_gdb_type (gdbarch, space->tdesc_type);

  return allocate_computed_value (type, &space_value_funcs, space);
}

/* When the architecture changes, remove any space variables for the
   old architecture, and add variables for the new architecture.  */
static void
tdesc_arch_changed_observer (struct gdbarch *old, struct gdbarch *new)
{
  const struct target_desc *old_tdesc = gdbarch_target_desc (old);
  const struct target_desc *new_tdesc = gdbarch_target_desc (new);
  struct tdesc_feature *feature;
  struct tdesc_space *space;
  int ix, ixs;

  if (old_tdesc == new_tdesc)
    return;

  /* FIXME: at the moment, GDB don't have a function for deleting
     internal variables.  We need to add one, and then clean up the
     old architecture's variables here.  */
  if (new_tdesc)
    for (ix = 0;
	 VEC_iterate (tdesc_feature_p, new_tdesc->features, ix, feature);
	 ix++)
      for (ixs = 0;
	   VEC_iterate (tdesc_space_p, feature->spaces, ixs, space);
	   ixs++)
        set_internalvar_lazy (lookup_internalvar (space->name),
                              make_space_value (new, feature, space));
}

void
tdesc_push_space_names (const struct target_desc *tdesc,
			VEC(char_p) **names)
{
  struct tdesc_feature *feature;
  struct tdesc_space *space;
  int ix, ixs;

  for (ix = 0;
       VEC_iterate (tdesc_feature_p, tdesc->features, ix, feature);
       ix++)
    for (ixs = 0;
	 VEC_iterate (tdesc_space_p, feature->spaces, ixs, space);
	 ixs++)
      VEC_safe_push (char_p, *names, space->name);
}


static struct cmd_list_element *tdesc_set_cmdlist, *tdesc_show_cmdlist;
static struct cmd_list_element *tdesc_unset_cmdlist;

/* Helper functions for the CLI commands.  */

static void
set_tdesc_cmd (char *args, int from_tty)
{
  help_list (tdesc_set_cmdlist, "set tdesc ", -1, gdb_stdout);
}

static void
show_tdesc_cmd (char *args, int from_tty)
{
  cmd_show_list (tdesc_show_cmdlist, from_tty, "");
}

static void
unset_tdesc_cmd (char *args, int from_tty)
{
  help_list (tdesc_unset_cmdlist, "unset tdesc ", -1, gdb_stdout);
}

static void
set_tdesc_filename_cmd (char *args, int from_tty,
			struct cmd_list_element *c)
{
  target_clear_description ();
  target_find_description ();
}

static void
show_tdesc_filename_cmd (struct ui_file *file, int from_tty,
			 struct cmd_list_element *c,
			 const char *value)
{
  if (value != NULL && *value != '\0')
    printf_filtered (_("\
The target description will be read from \"%s\".\n"),
		     value);
  else
    printf_filtered (_("\
The target description will be read from the target.\n"));
}

static void
unset_tdesc_filename_cmd (char *args, int from_tty)
{
  xfree (target_description_filename);
  target_description_filename = NULL;
  target_clear_description ();
  target_find_description ();
}

static void
maint_print_c_tdesc_cmd (char *args, int from_tty)
{
  const struct target_desc *tdesc;
  const struct bfd_arch_info *compatible;
  const char *filename, *inp;
  char *function, *outp;
  struct property *prop;
  struct tdesc_feature *feature;
  struct tdesc_reg *reg;
  struct tdesc_type *type;
  struct tdesc_type_field *f;
  struct tdesc_type_flag *flag;
  int ix, ix2, ix3;

  /* Use the global target-supplied description, not the current
     architecture's.  This lets a GDB for one architecture generate C
     for another architecture's description, even though the gdbarch
     initialization code will reject the new description.  */
  tdesc = current_target_desc;
  if (tdesc == NULL)
    error (_("There is no target description to print."));

  if (target_description_filename == NULL)
    error (_("The current target description did not come from an XML file."));

  filename = lbasename (target_description_filename);
  function = alloca (strlen (filename) + 1);
  for (inp = filename, outp = function; *inp != '\0'; inp++)
    if (*inp == '.')
      break;
    else if (*inp == '-')
      *outp++ = '_';
    else
      *outp++ = *inp;
  *outp = '\0';

  /* Standard boilerplate.  */
  printf_unfiltered ("/* THIS FILE IS GENERATED.  Original: %s */\n\n",
		     filename);
  printf_unfiltered ("#include \"defs.h\"\n");
  printf_unfiltered ("#include \"osabi.h\"\n");
  printf_unfiltered ("#include \"target-descriptions.h\"\n");
  printf_unfiltered ("\n");

  printf_unfiltered ("struct target_desc *tdesc_%s;\n", function);
  printf_unfiltered ("static void\n");
  printf_unfiltered ("initialize_tdesc_%s (void)\n", function);
  printf_unfiltered ("{\n");
  printf_unfiltered
    ("  struct target_desc *result = allocate_target_description ();\n");
  printf_unfiltered ("  struct tdesc_feature *feature;\n");
  printf_unfiltered ("  struct tdesc_type *field_type, *type;\n");
  printf_unfiltered ("\n");

  if (tdesc_architecture (tdesc) != NULL)
    {
      printf_unfiltered
	("  set_tdesc_architecture (result, bfd_scan_arch (\"%s\"));\n",
	 tdesc_architecture (tdesc)->printable_name);
      printf_unfiltered ("\n");
    }

  if (tdesc_osabi (tdesc) > GDB_OSABI_UNKNOWN
      && tdesc_osabi (tdesc) < GDB_OSABI_INVALID)
    {
      printf_unfiltered
	("  set_tdesc_osabi (result, osabi_from_tdesc_string (\"%s\"));\n",
	 gdbarch_osabi_name (tdesc_osabi (tdesc)));
      printf_unfiltered ("\n");
    }

  for (ix = 0; VEC_iterate (arch_p, tdesc->compatible, ix, compatible);
       ix++)
    {
      printf_unfiltered
	("  tdesc_add_compatible (result, bfd_scan_arch (\"%s\"));\n",
	 compatible->printable_name);
    }
  if (ix)
    printf_unfiltered ("\n");

  for (ix = 0; VEC_iterate (property_s, tdesc->properties, ix, prop);
       ix++)
    {
      printf_unfiltered ("  set_tdesc_property (result, \"%s\", \"%s\");\n",
	      prop->key, prop->value);
    }

  for (ix = 0;
       VEC_iterate (tdesc_feature_p, tdesc->features, ix, feature);
       ix++)
    {
      printf_unfiltered ("  feature = tdesc_create_feature (result, \"%s\");\n",
			 feature->name);

      for (ix2 = 0;
	   VEC_iterate (tdesc_type_p, feature->types, ix2, type);
	   ix2++)
	{
	  switch (type->kind)
	    {
	    case TDESC_TYPE_VECTOR:
	      printf_unfiltered
		("  field_type = tdesc_named_type (feature, \"%s\");\n",
		 type->u.v.type->name);
	      printf_unfiltered
		("  tdesc_create_vector (feature, \"%s\", field_type, %d);\n",
		 type->name, type->u.v.count);
	      break;
	    case TDESC_TYPE_UNION:
	      printf_unfiltered
		("  type = tdesc_create_union (feature, \"%s\");\n",
		 type->name);
	      for (ix3 = 0;
		   VEC_iterate (tdesc_type_field, type->u.u.fields, ix3, f);
		   ix3++)
		{
		  printf_unfiltered
		    ("  field_type = tdesc_named_type (feature, \"%s\");\n",
		     f->type->name);
		  printf_unfiltered
		    ("  tdesc_add_field (type, \"%s\", field_type);\n",
		     f->name);
		}
	      break;
	    case TDESC_TYPE_FLAGS:
	      printf_unfiltered
		("  field_type = tdesc_create_flags (feature, \"%s\", %d);\n",
		 type->name, (int) type->u.f.size);
	      for (ix3 = 0;
		   VEC_iterate (tdesc_type_flag, type->u.f.flags, ix3,
				flag);
		   ix3++)
		printf_unfiltered
		  ("  tdesc_add_flag (field_type, %d, \"%s\");\n",
		   flag->start, flag->name);
	      break;
	    default:
	      error (_("C output is not supported type \"%s\"."), type->name);
	    }
	  printf_unfiltered ("\n");
	}

      for (ix2 = 0;
	   VEC_iterate (tdesc_reg_p, feature->registers, ix2, reg);
	   ix2++)
	{
	  printf_unfiltered ("  tdesc_create_reg (feature, \"%s\", %ld, %d, ",
			     reg->name, reg->target_regnum, reg->save_restore);
	  if (reg->group)
	    printf_unfiltered ("\"%s\", ", reg->group);
	  else
	    printf_unfiltered ("NULL, ");
	  printf_unfiltered ("%d, \"%s\");\n", reg->bitsize, reg->type);
	}

      printf_unfiltered ("\n");
    }

  printf_unfiltered ("  tdesc_%s = result;\n", function);
  printf_unfiltered ("}\n");
}

/* Provide a prototype to silence -Wmissing-prototypes.  */
extern initialize_file_ftype _initialize_target_descriptions;

void
_initialize_target_descriptions (void)
{
  tdesc_data = gdbarch_data_register_pre_init (tdesc_data_init);

  observer_attach_arch_changed (tdesc_arch_changed_observer);

  add_prefix_cmd ("tdesc", class_maintenance, set_tdesc_cmd, _("\
Set target description specific variables."),
		  &tdesc_set_cmdlist, "set tdesc ",
		  0 /* allow-unknown */, &setlist);
  add_prefix_cmd ("tdesc", class_maintenance, show_tdesc_cmd, _("\
Show target description specific variables."),
		  &tdesc_show_cmdlist, "show tdesc ",
		  0 /* allow-unknown */, &showlist);
  add_prefix_cmd ("tdesc", class_maintenance, unset_tdesc_cmd, _("\
Unset target description specific variables."),
		  &tdesc_unset_cmdlist, "unset tdesc ",
		  0 /* allow-unknown */, &unsetlist);

  add_setshow_filename_cmd ("filename", class_obscure,
			    &target_description_filename,
			    _("\
Set the file to read for an XML target description"), _("\
Show the file to read for an XML target description"), _("\
When set, GDB will read the target description from a local\n\
file instead of querying the remote target."),
			    set_tdesc_filename_cmd,
			    show_tdesc_filename_cmd,
			    &tdesc_set_cmdlist, &tdesc_show_cmdlist);

  add_cmd ("filename", class_obscure, unset_tdesc_filename_cmd, _("\
Unset the file to read for an XML target description.  When unset,\n\
GDB will read the description from the target."),
	   &tdesc_unset_cmdlist);

  add_cmd ("c-tdesc", class_maintenance, maint_print_c_tdesc_cmd, _("\
Print the current target description as a C source file."),
	   &maintenanceprintlist);
}
