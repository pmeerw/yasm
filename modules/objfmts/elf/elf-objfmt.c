/*
 * ELF object format
 *
 *  Copyright (C) 2003  Michael Urman
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND OTHER CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR OTHER CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <util.h>
/*@unused@*/ RCSID("$Id$");

/* Notes
 *
 * elf-objfmt uses the "linking" view of an ELF file:
 * ELF header, an optional program header table, several sections,
 * and a section header table
 *
 * The ELF header tells us some overall program information,
 *   where to find the PHT (if it exists) with phnum and phentsize, 
 *   and where to find the SHT with shnum and shentsize
 *
 * The PHT doesn't seem to be generated by NASM for elftest.asm
 *
 * The SHT
 *
 * Each Section is spatially disjoint, and has exactly one SHT entry.
 */

#define YASM_LIB_INTERNAL
#define YASM_BC_INTERNAL
#define YASM_EXPR_INTERNAL
#include <libyasm.h>

#include "elf.h"
#include "elf-machine.h"

typedef struct yasm_objfmt_elf {
    yasm_objfmt_base objfmt;		/* base structure */

    elf_symtab_head* elf_symtab;	/* symbol table of indexed syms */
    elf_strtab_head* shstrtab;		/* section name strtab */
    elf_strtab_head* strtab;		/* strtab entries */

    yasm_object *object;
    yasm_symtab *symtab;
    /*@dependent@*/ yasm_arch *arch;
    
    elf_strtab_entry *file_strtab_entry;/* .file symbol associated string */
    yasm_symrec *dotdotsym;		/* ..sym symbol */
} yasm_objfmt_elf;

typedef struct {
    yasm_objfmt_elf *objfmt_elf;
    FILE *f;
    elf_secthead *shead;
    yasm_section *sect;
    yasm_object *object;
    unsigned long sindex;
} elf_objfmt_output_info;

typedef struct {
    yasm_objfmt_elf *objfmt_elf;
    int local_names;
} append_local_sym_info;

yasm_objfmt_module yasm_elf_LTX_objfmt;
yasm_objfmt_module yasm_elf32_LTX_objfmt;
yasm_objfmt_module yasm_elf64_LTX_objfmt;


static elf_symtab_entry *
elf_objfmt_symtab_append(yasm_objfmt_elf *objfmt_elf, yasm_symrec *sym,
			 elf_section_index sectidx, elf_symbol_binding bind,
			 elf_symbol_type type, elf_symbol_vis vis,
                         yasm_expr *size, elf_address *value)
{
    /* Only append to table if not already appended */
    elf_symtab_entry *entry = yasm_symrec_get_data(sym, &elf_symrec_data);
    if (!entry || !elf_sym_in_table(entry)) {
	if (!entry) {
	    elf_strtab_entry *name =
		elf_strtab_append_str(objfmt_elf->strtab,
				      yasm_symrec_get_name(sym));
	    entry = elf_symtab_entry_create(name, sym);
	}
	elf_symtab_append_entry(objfmt_elf->elf_symtab, entry);
	yasm_symrec_add_data(sym, &elf_symrec_data, entry);
    }

    elf_symtab_set_nonzero(entry, NULL, sectidx, bind, type, size, value);
    elf_sym_set_visibility(entry, vis);

    return entry;
}

static int
elf_objfmt_append_local_sym(yasm_symrec *sym, /*@null@*/ void *d)
{
    append_local_sym_info *info = (append_local_sym_info *)d;
    elf_symtab_entry *entry;
    elf_address value=0;
    yasm_section *sect=NULL;
    yasm_bytecode *precbc=NULL;

    assert(info != NULL);

    if (!yasm_symrec_get_label(sym, &precbc))
	return 0;
    sect = yasm_bc_get_section(precbc);

    entry = yasm_symrec_get_data(sym, &elf_symrec_data);
    if (!entry || !elf_sym_in_table(entry)) {
	int is_sect = 0;
	if (!yasm_section_is_absolute(sect) &&
	    strcmp(yasm_symrec_get_name(sym), yasm_section_get_name(sect))==0)
	    is_sect = 1;

	/* neither sections nor locals (except when debugging) need names */
	if (!entry) {
	    elf_strtab_entry *name = info->local_names && !is_sect
		? elf_strtab_append_str(info->objfmt_elf->strtab,
					yasm_symrec_get_name(sym))
		: NULL;
	    entry = elf_symtab_entry_create(name, sym);
	}
	elf_symtab_insert_local_sym(info->objfmt_elf->elf_symtab, entry);
	elf_symtab_set_nonzero(entry, sect, 0, STB_LOCAL,
			       is_sect ? STT_SECTION : 0, NULL, 0);
	yasm_symrec_add_data(sym, &elf_symrec_data, entry);

	if (is_sect)
	    return 0;
    }

    if (precbc)
	value = precbc->offset + precbc->len;
    elf_symtab_set_nonzero(entry, sect, 0, 0, 0, NULL, &value);

    return 0;
}

static yasm_objfmt *
elf_objfmt_create_common(yasm_object *object, yasm_arch *a,
			 yasm_objfmt_module *module, int bits_pref,
			 const elf_machine_handler **elf_march_out)
{
    yasm_objfmt_elf *objfmt_elf = yasm_xmalloc(sizeof(yasm_objfmt_elf));
    yasm_symrec *filesym;
    elf_symtab_entry *entry;
    const elf_machine_handler *elf_march;

    objfmt_elf->objfmt.module = module;
    objfmt_elf->object = object;
    objfmt_elf->symtab = yasm_object_get_symtab(object);
    objfmt_elf->arch = a;
    elf_march = elf_set_arch(a, objfmt_elf->symtab, bits_pref);
    if (!elf_march) {
	yasm_xfree(objfmt_elf);
	return NULL;
    }
    if (elf_march_out)
	*elf_march_out = elf_march;

    objfmt_elf->shstrtab = elf_strtab_create();
    objfmt_elf->strtab = elf_strtab_create();
    objfmt_elf->elf_symtab = elf_symtab_create();

    /* FIXME: misuse of NULL bytecode here; it works, but only barely. */
    filesym = yasm_symtab_define_label(objfmt_elf->symtab, ".file", NULL, 0,
				       0);
    /* Put in current input filename; we'll replace it in output() */
    objfmt_elf->file_strtab_entry =
	elf_strtab_append_str(objfmt_elf->strtab,
			      yasm_object_get_source_fn(object));
    entry = elf_symtab_entry_create(objfmt_elf->file_strtab_entry, filesym);
    yasm_symrec_add_data(filesym, &elf_symrec_data, entry);
    elf_symtab_set_nonzero(entry, NULL, SHN_ABS, STB_LOCAL, STT_FILE, NULL,
			   NULL);
    elf_symtab_append_entry(objfmt_elf->elf_symtab, entry);

    /* FIXME: misuse of NULL bytecode */
    objfmt_elf->dotdotsym = yasm_symtab_define_label(objfmt_elf->symtab,
						     "..sym", NULL, 1, 0);

    return (yasm_objfmt *)objfmt_elf;
}

static yasm_objfmt *
elf_objfmt_create(yasm_object *object, yasm_arch *a)
{
    const elf_machine_handler *elf_march;
    yasm_objfmt *objfmt;
    yasm_objfmt_elf *objfmt_elf;

    objfmt = elf_objfmt_create_common(object, a, &yasm_elf_LTX_objfmt, 0,
				      &elf_march);
    if (objfmt) {
	objfmt_elf = (yasm_objfmt_elf *)objfmt;
	/* Figure out which bitness of object format to use */
	if (elf_march->bits == 32)
	    objfmt_elf->objfmt.module = &yasm_elf32_LTX_objfmt;
	else if (elf_march->bits == 64)
	    objfmt_elf->objfmt.module = &yasm_elf64_LTX_objfmt;
    }
    return objfmt;
}

static yasm_objfmt *
elf32_objfmt_create(yasm_object *object, yasm_arch *a)
{
    return elf_objfmt_create_common(object, a, &yasm_elf32_LTX_objfmt, 32,
				    NULL);
}

static yasm_objfmt *
elf64_objfmt_create(yasm_object *object, yasm_arch *a)
{
    return elf_objfmt_create_common(object, a, &yasm_elf64_LTX_objfmt, 64,
				    NULL);
}

static long
elf_objfmt_output_align(FILE *f, unsigned int align)
{
    long pos;
    unsigned long delta;
    if ((align & (align-1)) != 0)
	yasm_internal_error("requested alignment not a power of two");

    pos = ftell(f);
    if (pos == -1) {
	yasm__error(0, N_("could not get file position on output file"));
	return -1;
    }
    delta = align - (pos & (align-1)); 
    if (delta != align) {
	pos += delta;
	if (fseek(f, pos, SEEK_SET) < 0) {
	    yasm__error(0, N_("could not set file position on output file"));
	    return -1;
	}
    }
    return pos;
}

static int
elf_objfmt_output_reloc(yasm_symrec *sym, yasm_bytecode *bc,
			unsigned char *buf, size_t destsize, size_t valsize,
			int warn, void *d)
{
    elf_reloc_entry *reloc;
    elf_objfmt_output_info *info = d;
    yasm_intnum *zero;
    int retval;

    reloc = elf_reloc_entry_create(sym, NULL,
	yasm_intnum_create_uint(bc->offset), 0, valsize);
    if (reloc == NULL) {
	yasm__error(bc->line, N_("elf: invalid relocation size"));
	return 1;
    }
    /* allocate .rel[a] sections on a need-basis */
    elf_secthead_append_reloc(info->sect, info->shead, reloc);

    zero = yasm_intnum_create_uint(0);
    elf_handle_reloc_addend(zero, reloc);
    retval = yasm_arch_intnum_tobytes(info->objfmt_elf->arch, zero, buf,
				      destsize, valsize, 0, bc, warn,
				      bc->line);
    yasm_intnum_destroy(zero);
    return retval;
}

static int
elf_objfmt_output_value(yasm_value *value, unsigned char *buf, size_t destsize,
			size_t valsize, int shift, unsigned long offset,
			yasm_bytecode *bc, int warn, /*@null@*/ void *d)
{
    /*@null@*/ elf_objfmt_output_info *info = (elf_objfmt_output_info *)d;
    /*@dependent@*/ /*@null@*/ yasm_intnum *intn;
    unsigned long intn_val;
    /*@null@*/ elf_reloc_entry *reloc = NULL;
    int retval;

    if (info == NULL)
	yasm_internal_error("null info struct");

    if (value->abs)
	value->abs = yasm_expr_simplify(value->abs, yasm_common_calc_bc_dist);

    /* Try to output constant and PC-relative section-local first.
     * Note this does NOT output any value with a SEG, WRT, external,
     * cross-section, or non-PC-relative reference (those are handled below).
     */
    switch (yasm_value_output_basic(value, buf, destsize, valsize, shift, bc,
				    warn, info->objfmt_elf->arch,
				    yasm_common_calc_bc_dist)) {
	case -1:
	    return 1;
	case 0:
	    break;
	default:
	    return 0;
    }

    /* Handle other expressions, with relocation if necessary */
    if (value->seg_of || value->section_rel || value->rshift > 0) {
	yasm__error(bc->line, N_("elf: relocation too complex"));
	return 1;
    }

    intn_val = 0;
    if (value->rel) {
	yasm_sym_vis vis = yasm_symrec_get_visibility(value->rel);
	/*@dependent@*/ /*@null@*/ yasm_symrec *sym = value->rel;
	/*@dependent@*/ /*@null@*/ yasm_symrec *wrt = value->wrt;

	if (wrt == info->objfmt_elf->dotdotsym)
	    wrt = NULL;
	else if (wrt && elf_is_wrt_sym_relative(wrt))
	    ;
	else if (vis == YASM_SYM_LOCAL) {
	    yasm_bytecode *sym_precbc;
	    /* Local symbols need relocation to their section's start, and
	     * add in the offset of the bytecode (within the target section)
	     * into the abs portion.
	     *
	     * This is only done if the symbol is relocated against the
	     * section instead of the symbol itself.
	     */
	    if (yasm_symrec_get_label(sym, &sym_precbc)) {
		/* Relocate to section start */
		yasm_section *sym_sect = yasm_bc_get_section(sym_precbc);
		/*@null@*/ elf_secthead *sym_shead;
		sym_shead = yasm_section_get_data(sym_sect, &elf_section_data);
		assert(sym_shead != NULL);
		sym = elf_secthead_get_sym(sym_shead);

		intn_val = sym_precbc->offset + sym_precbc->len;
	    }
	}
	
	/* For PC-relative, need to add offset of expression within bc. */
	if (value->curpos_rel)
	    intn_val += offset;

	reloc = elf_reloc_entry_create(sym, wrt,
	    yasm_intnum_create_uint(bc->offset + offset), value->curpos_rel,
	    valsize);
	if (reloc == NULL) {
	    yasm__error(bc->line, N_("elf: invalid relocation (WRT or size)"));
	    return 1;
	}
	/* allocate .rel[a] sections on a need-basis */
	elf_secthead_append_reloc(info->sect, info->shead, reloc);
    }

    intn = yasm_intnum_create_uint(intn_val);

    if (value->abs) {
	yasm_intnum *intn2 = yasm_expr_get_intnum(&value->abs, NULL);
	if (!intn2) {
	    yasm__error(bc->line, N_("elf: relocation too complex"));
	    yasm_intnum_destroy(intn);
	    return 1;
	}
	yasm_intnum_calc(intn, YASM_EXPR_ADD, intn2, bc->line);
    }

    if (reloc)
	elf_handle_reloc_addend(intn, reloc);
    retval = yasm_arch_intnum_tobytes(info->objfmt_elf->arch, intn, buf,
				      destsize, valsize, shift, bc, warn,
				      bc->line);
    yasm_intnum_destroy(intn);
    return retval;
}

static int
elf_objfmt_output_bytecode(yasm_bytecode *bc, /*@null@*/ void *d)
{
    /*@null@*/ elf_objfmt_output_info *info = (elf_objfmt_output_info *)d;
    unsigned char buf[256];
    /*@null@*/ /*@only@*/ unsigned char *bigbuf;
    unsigned long size = 256;
    int gap;

    if (info == NULL)
	yasm_internal_error("null info struct");

    bigbuf = yasm_bc_tobytes(bc, buf, &size, &gap, info,
			     elf_objfmt_output_value, elf_objfmt_output_reloc);

    /* Don't bother doing anything else if size ended up being 0. */
    if (size == 0) {
	if (bigbuf)
	    yasm_xfree(bigbuf);
	return 0;
    }
    else {
	yasm_intnum *bcsize = yasm_intnum_create_uint(size);
	elf_secthead_add_size(info->shead, bcsize);
	yasm_intnum_destroy(bcsize);
    }

    /* Warn that gaps are converted to 0 and write out the 0's. */
    if (gap) {
	unsigned long left;
	yasm__warning(YASM_WARN_UNINIT_CONTENTS, bc->line,
	    N_("uninitialized space declared in code/data section: zeroing"));
	/* Write out in chunks */
	memset(buf, 0, 256);
	left = size;
	while (left > 256) {
	    fwrite(buf, 256, 1, info->f);
	    left -= 256;
	}
	fwrite(buf, left, 1, info->f);
    } else {
	/* Output buf (or bigbuf if non-NULL) to file */
	fwrite(bigbuf ? bigbuf : buf, (size_t)size, 1, info->f);
    }

    /* If bigbuf was allocated, free it */
    if (bigbuf)
	yasm_xfree(bigbuf);

    return 0;
}

static int
elf_objfmt_create_dbg_secthead(yasm_section *sect, void *d)
{
    /*@null@*/ elf_objfmt_output_info *info = (elf_objfmt_output_info *)d;
    elf_secthead *shead;
    elf_section_type type=SHT_PROGBITS;
    elf_size entsize=0;
    const char *sectname;
    /*@dependent@*/ yasm_symrec *sym;
    elf_strtab_entry *name;

    shead = yasm_section_get_data(sect, &elf_section_data);
    if (yasm_section_is_absolute(sect) || shead)
	return 0;   /* only create new secthead if missing and non-absolute */

    sectname = yasm_section_get_name(sect);
    name = elf_strtab_append_str(info->objfmt_elf->shstrtab, sectname);

    if (yasm__strcasecmp(sectname, ".stab")==0) {
	entsize = 12;
    } else if (yasm__strcasecmp(sectname, ".stabstr")==0) {
	type = SHT_STRTAB;
    } else if (yasm__strncasecmp(sectname, ".debug_", 7)==0) {
	;
    } else
	yasm_internal_error(N_("Unrecognized section without data"));

    shead = elf_secthead_create(name, type, 0, 0, 0);
    elf_secthead_set_entsize(shead, entsize);

    sym = yasm_symtab_define_label(
	yasm_object_get_symtab(info->objfmt_elf->object), sectname,
	yasm_section_bcs_first(sect), 1, 0);
    elf_secthead_set_sym(shead, sym);

    yasm_section_add_data(sect, &elf_section_data, shead);

    return 0;
}

static int
elf_objfmt_output_section(yasm_section *sect, /*@null@*/ void *d)
{
    /*@null@*/ elf_objfmt_output_info *info = (elf_objfmt_output_info *)d;
    /*@dependent@*/ /*@null@*/ elf_secthead *shead;
    long pos;
    char *relname;
    const char *sectname;

    /* Don't output absolute sections into the section table */
    if (yasm_section_is_absolute(sect))
	return 0;

    if (info == NULL)
	yasm_internal_error("null info struct");
    shead = yasm_section_get_data(sect, &elf_section_data);
    if (shead == NULL)
	yasm_internal_error("no associated data");

    if (elf_secthead_get_align(shead) == 0)
	elf_secthead_set_align(shead, yasm_section_get_align(sect));

    /* don't output header-only sections */
    if ((elf_secthead_get_type(shead) & SHT_NOBITS) == SHT_NOBITS)
    {
	yasm_bytecode *last = yasm_section_bcs_last(sect);
	if (last) {
	    yasm_intnum *sectsize;
	    sectsize = yasm_intnum_create_uint(last->offset + last->len);
	    elf_secthead_add_size(shead, sectsize);
	    yasm_intnum_destroy(sectsize);
	}
	elf_secthead_set_index(shead, ++info->sindex);
	return 0;
    }

    if ((pos = ftell(info->f)) == -1)
	yasm__error(0, N_("couldn't read position on output stream"));
    pos = elf_secthead_set_file_offset(shead, pos);
    if (fseek(info->f, pos, SEEK_SET) < 0)
	yasm__error(0, N_("couldn't seek on output stream"));

    info->sect = sect;
    info->shead = shead;
    yasm_section_bcs_traverse(sect, info, elf_objfmt_output_bytecode);

    elf_secthead_set_index(shead, ++info->sindex);

    /* No relocations to output?  Go on to next section */
    if (elf_secthead_write_relocs_to_file(info->f, sect, shead) == 0)
	return 0;
    elf_secthead_set_rel_index(shead, ++info->sindex);

    /* name the relocation section .rel[a].foo */
    sectname = yasm_section_get_name(sect);
    relname = elf_secthead_name_reloc_section(sectname);
    elf_secthead_set_rel_name(shead,
        elf_strtab_append_str(info->objfmt_elf->shstrtab, relname));
    yasm_xfree(relname);

    return 0;
}

static int
elf_objfmt_output_secthead(yasm_section *sect, /*@null@*/ void *d)
{
    /*@null@*/ elf_objfmt_output_info *info = (elf_objfmt_output_info *)d;
    /*@dependent@*/ /*@null@*/ elf_secthead *shead;

    /* Don't output absolute sections into the section table */
    if (yasm_section_is_absolute(sect))
	return 0;

    if (info == NULL)
	yasm_internal_error("null info struct");
    shead = yasm_section_get_data(sect, &elf_section_data);
    if (shead == NULL)
	yasm_internal_error("no section header attached to section");

    if(elf_secthead_write_to_file(info->f, shead, info->sindex+1))
	info->sindex++;

    /* output strtab headers here? */

    /* relocation entries for .foo are stored in section .rel[a].foo */
    if(elf_secthead_write_rel_to_file(info->f, 3, sect, shead,
				      info->sindex+1))
	info->sindex++;

    return 0;
}

static void
elf_objfmt_output(yasm_objfmt *objfmt, FILE *f, int all_syms, yasm_dbgfmt *df)
{
    yasm_objfmt_elf *objfmt_elf = (yasm_objfmt_elf *)objfmt;
    elf_objfmt_output_info info;
    append_local_sym_info localsym_info;
    long pos;
    unsigned long elf_shead_addr;
    elf_secthead *esdn;
    unsigned long elf_strtab_offset, elf_shstrtab_offset, elf_symtab_offset;
    unsigned long elf_strtab_size, elf_shstrtab_size, elf_symtab_size;
    elf_strtab_entry *elf_strtab_name, *elf_shstrtab_name, *elf_symtab_name;
    unsigned long elf_symtab_nlocal;

    info.objfmt_elf = objfmt_elf;
    info.f = f;

    /* Update filename strtab */
    elf_strtab_entry_set_str(objfmt_elf->file_strtab_entry,
			     yasm_object_get_source_fn(objfmt_elf->object));

    /* Allocate space for Ehdr by seeking forward */
    if (fseek(f, (long)(elf_proghead_get_size()), SEEK_SET) < 0) {
	yasm__error(0, N_("could not seek on output file"));
	return;
    }

    /* Create missing section headers */
    localsym_info.objfmt_elf = objfmt_elf;
    if (yasm_object_sections_traverse(objfmt_elf->object, &info,
				      elf_objfmt_create_dbg_secthead))
	return;

    /* add all (local) syms to symtab because relocation needs a symtab index
     * if all_syms, register them by name.  if not, use strtab entry 0 */
    localsym_info.local_names = all_syms;
    yasm_symtab_traverse(yasm_object_get_symtab(objfmt_elf->object),
			 &localsym_info, elf_objfmt_append_local_sym);
    elf_symtab_nlocal = elf_symtab_assign_indices(objfmt_elf->elf_symtab);

    /* output known sections - includes reloc sections which aren't in yasm's
     * list.  Assign indices as we go. */
    info.sindex = 3;
    if (yasm_object_sections_traverse(objfmt_elf->object, &info,
				      elf_objfmt_output_section))
	return;

    /* add final sections to the shstrtab */
    elf_strtab_name = elf_strtab_append_str(objfmt_elf->shstrtab, ".strtab");
    elf_symtab_name = elf_strtab_append_str(objfmt_elf->shstrtab, ".symtab");
    elf_shstrtab_name = elf_strtab_append_str(objfmt_elf->shstrtab,
					      ".shstrtab");

    /* output .shstrtab */
    if ((pos = elf_objfmt_output_align(f, 4)) == -1)
	return;
    elf_shstrtab_offset = (unsigned long) pos;
    elf_shstrtab_size = elf_strtab_output_to_file(f, objfmt_elf->shstrtab);

    /* output .strtab */
    if ((pos = elf_objfmt_output_align(f, 4)) == -1)
	return;
    elf_strtab_offset = (unsigned long) pos;
    elf_strtab_size = elf_strtab_output_to_file(f, objfmt_elf->strtab);

    /* output .symtab - last section so all others have indexes */
    if ((pos = elf_objfmt_output_align(f, 4)) == -1)
	return;
    elf_symtab_offset = (unsigned long) pos;
    elf_symtab_size = elf_symtab_write_to_file(f, objfmt_elf->elf_symtab);

    /* output section header table */
    if ((pos = elf_objfmt_output_align(f, 16)) == -1)
	return;
    elf_shead_addr = (unsigned long) pos;

    /* stabs debugging support */
    if (strcmp(yasm_dbgfmt_keyword(df), "stabs")==0) {
	yasm_section *stabsect = yasm_object_find_general(objfmt_elf->object,
							  ".stab");
	yasm_section *stabstrsect =
	    yasm_object_find_general(objfmt_elf->object, ".stabstr");
	if (stabsect && stabstrsect) {
	    elf_secthead *stab =
		yasm_section_get_data(stabsect, &elf_section_data);
	    elf_secthead *stabstr =
		yasm_section_get_data(stabstrsect, &elf_section_data);
	    if (stab && stabstr) {
		elf_secthead_set_link(stab, elf_secthead_get_index(stabstr));
	    }
	    else
		yasm_internal_error(N_("missing .stab or .stabstr section/data"));
	}
    }
    
    /* output dummy section header - 0 */
    info.sindex = 0;

    esdn = elf_secthead_create(NULL, SHT_NULL, 0, 0, 0);
    elf_secthead_set_index(esdn, 0);
    elf_secthead_write_to_file(f, esdn, 0);
    elf_secthead_destroy(esdn);

    esdn = elf_secthead_create(elf_shstrtab_name, SHT_STRTAB, 0,
			       elf_shstrtab_offset, elf_shstrtab_size);
    elf_secthead_set_index(esdn, 1);
    elf_secthead_write_to_file(f, esdn, 1);
    elf_secthead_destroy(esdn);

    esdn = elf_secthead_create(elf_strtab_name, SHT_STRTAB, 0,
			       elf_strtab_offset, elf_strtab_size);
    elf_secthead_set_index(esdn, 2);
    elf_secthead_write_to_file(f, esdn, 2);
    elf_secthead_destroy(esdn);

    esdn = elf_secthead_create(elf_symtab_name, SHT_SYMTAB, 0,
			       elf_symtab_offset, elf_symtab_size);
    elf_secthead_set_index(esdn, 3);
    elf_secthead_set_info(esdn, elf_symtab_nlocal);
    elf_secthead_set_link(esdn, 2);	/* for .strtab, which is index 2 */
    elf_secthead_write_to_file(f, esdn, 3);
    elf_secthead_destroy(esdn);

    info.sindex = 3;
    /* output remaining section headers */
    yasm_object_sections_traverse(objfmt_elf->object, &info,
				  elf_objfmt_output_secthead);

    /* output Ehdr */
    if (fseek(f, 0, SEEK_SET) < 0) {
	yasm__error(0, N_("could not seek on output file"));
	return;
    }

    elf_proghead_write_to_file(f, elf_shead_addr, info.sindex+1, 1);
}

static void
elf_objfmt_destroy(yasm_objfmt *objfmt)
{
    yasm_objfmt_elf *objfmt_elf = (yasm_objfmt_elf *)objfmt;
    elf_symtab_destroy(objfmt_elf->elf_symtab);
    elf_strtab_destroy(objfmt_elf->shstrtab);
    elf_strtab_destroy(objfmt_elf->strtab);
    yasm_xfree(objfmt);
}

static elf_secthead *
elf_objfmt_init_new_section(yasm_objfmt_elf *objfmt_elf, yasm_section *sect,
			    const char *sectname, unsigned long type,
			    unsigned long flags, unsigned long line)
{
    elf_secthead *esd;
    yasm_symrec *sym;
    elf_strtab_entry *name = elf_strtab_append_str(objfmt_elf->shstrtab,
						   sectname);

    esd = elf_secthead_create(name, type, flags, 0, 0);
    yasm_section_add_data(sect, &elf_section_data, esd);
    sym = yasm_symtab_define_label(
	yasm_object_get_symtab(objfmt_elf->object), sectname,
	yasm_section_bcs_first(sect), 1, line);

    elf_secthead_set_sym(esd, sym);

    return esd;
}

static yasm_section *
elf_objfmt_add_default_section(yasm_objfmt *objfmt)
{
    yasm_objfmt_elf *objfmt_elf = (yasm_objfmt_elf *)objfmt;
    yasm_section *retval;
    int isnew;
    elf_secthead *esd;

    retval = yasm_object_get_general(objfmt_elf->object, ".text", 0, 16, 1, 0,
				     &isnew, 0);
    esd = elf_objfmt_init_new_section(objfmt_elf, retval, ".text", SHT_PROGBITS,
				      SHF_ALLOC + SHF_EXECINSTR, 0);
    yasm_section_set_default(retval, 1);
    return retval;
}

static /*@observer@*/ /*@null@*/ yasm_section *
elf_objfmt_section_switch(yasm_objfmt *objfmt, yasm_valparamhead *valparams,
			  /*@null@*/ yasm_valparamhead *objext_valparams,
			  unsigned long line)
{
    yasm_objfmt_elf *objfmt_elf = (yasm_objfmt_elf *)objfmt;
    yasm_valparam *vp = yasm_vps_first(valparams);
    yasm_section *retval;
    int isnew;
    unsigned long type = SHT_PROGBITS;
    unsigned long flags = SHF_ALLOC;
    unsigned long align = 4;
    int flags_override = 0;
    char *sectname;
    int resonly = 0;
    static const struct {
	const char *name;
	unsigned long flags;
    } flagquals[] = {
	{ "alloc",	SHF_ALLOC },
	{ "exec",	SHF_EXECINSTR },
	{ "write",	SHF_WRITE },
	/*{ "progbits",	SHT_PROGBITS },*/
	/*{ "align",	0 } */
    };
    /*@dependent@*/ /*@null@*/ const yasm_intnum *merge_intn = NULL;
    elf_secthead *esd;

    if (!vp || vp->param || !vp->val)
	return NULL;

    sectname = vp->val;

    if (strcmp(sectname, ".bss") == 0) {
	type = SHT_NOBITS;
	flags = SHF_ALLOC + SHF_WRITE;
	resonly = 1;
    } else if (strcmp(sectname, ".data") == 0) {
	type = SHT_PROGBITS;
	flags = SHF_ALLOC + SHF_WRITE;
    } else if (strcmp(sectname, ".rodata") == 0) {
	type = SHT_PROGBITS;
	flags = SHF_ALLOC;
    } else if (strcmp(sectname, ".text") == 0) {
	align = 16;
	type = SHT_PROGBITS;
	flags = SHF_ALLOC + SHF_EXECINSTR;
    } else if (strcmp(sectname, ".comment") == 0) {
	align = 0;
	type = SHT_PROGBITS;
	flags = 0;
    } else {
	/* Default to code */
	align = 1;
    }

    while ((vp = yasm_vps_next(vp))) {
	size_t i;
	int match;

	if (!vp->val) {
	    yasm__warning(YASM_WARN_GENERAL, line,
			  N_("Unrecognized numeric qualifier"));
	    continue;
	}

	match = 0;
	for (i=0; i<NELEMS(flagquals) && !match; i++) {
	    if (yasm__strcasecmp(vp->val, flagquals[i].name) == 0) {
		flags_override = 1;
		match = 1;
		flags |= flagquals[i].flags;
	    }
	    else if (yasm__strcasecmp(vp->val+2, flagquals[i].name) == 0
		  && yasm__strncasecmp(vp->val, "no", 2) == 0) {
		flags &= ~flagquals[i].flags;
		flags_override = 1;
		match = 1;
	    }
	}

	if (match)
	    ;
	else if (yasm__strncasecmp(vp->val, "gas_", 4) == 0) {
	    /* GAS-style flags */
	    flags = 0;
	    for (i=4; i<strlen(vp->val); i++) {
		switch (vp->val[i]) {
		    case 'a':
			flags |= SHF_ALLOC;
			break;
		    case 'w':
			flags |= SHF_WRITE;
			break;
		    case 'x':
			flags |= SHF_EXECINSTR;
			break;
		    case 'M':
			flags |= SHF_MERGE;
			break;
		    case 'S':
			flags |= SHF_STRINGS;
			break;
		    case 'G':
			flags |= SHF_GROUP;
			break;
		    case 'T':
			flags |= SHF_TLS;
			break;
		    default:
			yasm__warning(YASM_WARN_GENERAL, line,
				      N_("unrecognized section attribute: `%c'"),
				      vp->val[i]);
		}
	    }
	} else if (yasm__strcasecmp(vp->val, "progbits") == 0) {
	    type |= SHT_PROGBITS;
	}
	else if (yasm__strcasecmp(vp->val, "noprogbits") == 0 ||
		 yasm__strcasecmp(vp->val, "nobits") == 0) {
	    type &= ~SHT_PROGBITS;
	    type |= SHT_NOBITS;
	}
	else if (yasm__strcasecmp(vp->val, "align") == 0 && vp->param) {
            /*@dependent@*/ /*@null@*/ const yasm_intnum *align_expr;

            align_expr = yasm_expr_get_intnum(&vp->param, NULL);
            if (!align_expr) {
                yasm__error(line,
                            N_("argument to `%s' is not a power of two"),
                            vp->val);
                return NULL;
            }
            align = yasm_intnum_get_uint(align_expr);

            /* Alignments must be a power of two. */
            if ((align & (align - 1)) != 0) {
                yasm__error(line,
                            N_("argument to `%s' is not a power of two"),
                            vp->val);
                return NULL;
            }
	} else
	    yasm__warning(YASM_WARN_GENERAL, line,
			  N_("Unrecognized qualifier `%s'"), vp->val);
    }
	/* Handle merge entity size */
	if (flags & SHF_MERGE) {
	    if (objext_valparams && (vp = yasm_vps_first(objext_valparams))
		&& vp->param) {

		merge_intn = yasm_expr_get_intnum(&vp->param, NULL);
		if (!merge_intn)
		    yasm__warning(YASM_WARN_GENERAL, line,
				  N_("invalid merge entity size"));
	    } else {
		yasm__warning(YASM_WARN_GENERAL, line,
			      N_("entity size for SHF_MERGE not specified"));
		flags &= ~SHF_MERGE;
	    }
	}

    retval = yasm_object_get_general(objfmt_elf->object, sectname, 0, align,
				     (flags & SHF_EXECINSTR) != 0, resonly,
				     &isnew, line);

    if (isnew)
	esd = elf_objfmt_init_new_section(objfmt_elf, retval, sectname, type,
					  flags, line);
    else
	esd = yasm_section_get_data(retval, &elf_section_data);

    if (isnew || yasm_section_is_default(retval)) {
	yasm_section_set_default(retval, 0);
	elf_secthead_set_typeflags(esd, type, flags);
	if (merge_intn)
	    elf_secthead_set_entsize(esd, yasm_intnum_get_uint(merge_intn));
	yasm_section_set_align(retval, align, line);
    } else if (flags_override)
	yasm__warning(YASM_WARN_GENERAL, line,
		      N_("section flags ignored on section redeclaration"));
    return retval;
}

static yasm_symrec *
elf_objfmt_extern_declare(yasm_objfmt *objfmt, const char *name, /*@unused@*/
			  /*@null@*/ yasm_valparamhead *objext_valparams,
			  unsigned long line)
{
    yasm_objfmt_elf *objfmt_elf = (yasm_objfmt_elf *)objfmt;
    yasm_symrec *sym;

    sym = yasm_symtab_declare(objfmt_elf->symtab, name, YASM_SYM_EXTERN, line);
    elf_objfmt_symtab_append(objfmt_elf, sym, SHN_UNDEF, STB_GLOBAL,
                             0, STV_DEFAULT, NULL, NULL);

    if (objext_valparams) {
	yasm_valparam *vp = yasm_vps_first(objext_valparams);
	for (; vp; vp = yasm_vps_next(vp))
        {
            if (vp->val)
                yasm__error(line, N_("unrecognized symbol type `%s'"), vp->val);
        }
    }
    return sym;
}

static yasm_symrec *
elf_objfmt_global_declare(yasm_objfmt *objfmt, const char *name,
			  /*@null@*/ yasm_valparamhead *objext_valparams,
			  unsigned long line)
{
    yasm_objfmt_elf *objfmt_elf = (yasm_objfmt_elf *)objfmt;
    yasm_symrec *sym;
    elf_symbol_type type = 0;
    yasm_expr *size = NULL;
    elf_symbol_vis vis = STV_DEFAULT;
    unsigned int vis_overrides = 0;

    sym = yasm_symtab_declare(objfmt_elf->symtab, name, YASM_SYM_GLOBAL, line);

    if (objext_valparams) {
	yasm_valparam *vp = yasm_vps_first(objext_valparams);
	for (; vp; vp = yasm_vps_next(vp))
        {
            if (vp->val) {
                if (yasm__strcasecmp(vp->val, "function") == 0)
                    type = STT_FUNC;
                else if (yasm__strcasecmp(vp->val, "data") == 0 ||
                         yasm__strcasecmp(vp->val, "object") == 0)
                    type = STT_OBJECT;
                else if (yasm__strcasecmp(vp->val, "internal") == 0) {
                    vis = STV_INTERNAL;
                    vis_overrides++;
                }
                else if (yasm__strcasecmp(vp->val, "hidden") == 0) {
                    vis = STV_HIDDEN;
                    vis_overrides++;
                }
                else if (yasm__strcasecmp(vp->val, "protected") == 0) {
                    vis = STV_PROTECTED;
                    vis_overrides++;
                }
                else
                    yasm__error(line, N_("unrecognized symbol type `%s'"),
                                vp->val);
            }
            else if (vp->param && !size) {
                size = vp->param;
                vp->param = NULL;	/* to avoid deleting the expr */
            }
	}
        if (vis_overrides > 1) {
            yasm__warning(YASM_WARN_GENERAL, line,
                N_("More than one symbol visibility provided; using last"));
        }
    }

    elf_objfmt_symtab_append(objfmt_elf, sym, SHN_UNDEF, STB_GLOBAL,
                             type, vis, size, NULL);

    return sym;
}

static yasm_symrec *
elf_objfmt_common_declare(yasm_objfmt *objfmt, const char *name,
			  /*@only@*/ yasm_expr *size, /*@null@*/
			  yasm_valparamhead *objext_valparams,
			  unsigned long line)
{
    yasm_objfmt_elf *objfmt_elf = (yasm_objfmt_elf *)objfmt;
    yasm_symrec *sym;
    unsigned long addralign = 0;

    sym = yasm_symtab_declare(objfmt_elf->symtab, name, YASM_SYM_COMMON, line);

    if (objext_valparams) {
	yasm_valparam *vp = yasm_vps_first(objext_valparams);
        for (; vp; vp = yasm_vps_next(vp)) {
            if (!vp->val && vp->param) {
                /*@dependent@*/ /*@null@*/ const yasm_intnum *align_expr;

                align_expr = yasm_expr_get_intnum(&vp->param, NULL);
                if (!align_expr) {
                    yasm__error(line,
                                N_("alignment constraint is not a power of two"));
                    return sym;
                }
                addralign = yasm_intnum_get_uint(align_expr);

                /* Alignments must be a power of two. */
                if ((addralign & (addralign - 1)) != 0) {
                    yasm__error(line,
                                N_("alignment constraint is not a power of two"));
                    return sym;
                }
            } else if (vp->val)
                yasm__warning(YASM_WARN_GENERAL, line,
                              N_("Unrecognized qualifier `%s'"), vp->val);
        }
    }

    elf_objfmt_symtab_append(objfmt_elf, sym, SHN_COMMON, STB_GLOBAL,
                             0, STV_DEFAULT, size, &addralign);

    return sym;
}

static int
elf_objfmt_directive(yasm_objfmt *objfmt, const char *name,
		     yasm_valparamhead *valparams,
		     /*@unused@*/ /*@null@*/
		     yasm_valparamhead *objext_valparams,
		     unsigned long line)
{
    yasm_objfmt_elf *objfmt_elf = (yasm_objfmt_elf *)objfmt;
    yasm_symrec *sym;
    yasm_valparam *vp = yasm_vps_first(valparams);
    char *symname = vp->val;
    elf_symtab_entry *entry;

    if (!symname) {
	yasm__error(line, N_("Symbol name not specified"));
	return 0;
    }

    if (yasm__strcasecmp(name, "type") == 0) {
	/* Get symbol elf data */
	sym = yasm_symtab_use(objfmt_elf->symtab, symname, line);
	entry = yasm_symrec_get_data(sym, &elf_symrec_data);

	/* Create entry if necessary */
	if (!entry) {
	    entry = elf_symtab_entry_create(
		elf_strtab_append_str(objfmt_elf->strtab, symname), sym);
	    yasm_symrec_add_data(sym, &elf_symrec_data, entry);
	}

	/* Pull new type from val */
	vp = yasm_vps_next(vp);
	if (vp->val) {
	    if (yasm__strcasecmp(vp->val, "function") == 0)
		elf_sym_set_type(entry, STT_FUNC);
	    else if (yasm__strcasecmp(vp->val, "object") == 0)
		elf_sym_set_type(entry, STT_OBJECT);
	    else
		yasm__warning(YASM_WARN_GENERAL, line,
			      N_("unrecognized symbol type `%s'"), vp->val);
	} else
	    yasm__error(line, N_("no type specified"));
    } else if (yasm__strcasecmp(name, "size") == 0) {
	/* Get symbol elf data */
	sym = yasm_symtab_use(objfmt_elf->symtab, symname, line);
	entry = yasm_symrec_get_data(sym, &elf_symrec_data);

	/* Create entry if necessary */
	if (!entry) {
	    entry = elf_symtab_entry_create(
		elf_strtab_append_str(objfmt_elf->strtab, symname), sym);
	    yasm_symrec_add_data(sym, &elf_symrec_data, entry);
	}

	/* Pull new size from either param (expr) or val */
	vp = yasm_vps_next(vp);
	if (vp->param) {
	    elf_sym_set_size(entry, vp->param);
	    vp->param = NULL;
	} else if (vp->val)
	    elf_sym_set_size(entry, yasm_expr_create_ident(yasm_expr_sym(
		yasm_symtab_use(objfmt_elf->symtab, vp->val, line)), line));
	else
	    yasm__error(line, N_("no size specified"));
    } else if (yasm__strcasecmp(name, "weak") == 0) {
	sym = yasm_symtab_declare(objfmt_elf->symtab, symname, YASM_SYM_GLOBAL,
				  line);
	elf_objfmt_symtab_append(objfmt_elf, sym, SHN_UNDEF, STB_WEAK,
				 0, STV_DEFAULT, NULL, NULL);
    } else
	return 1;	/* unrecognized */

    return 0;
}


/* Define valid debug formats to use with this object format */
static const char *elf_objfmt_dbgfmt_keywords[] = {
    "null",
    "stabs",
    "dwarf2",
    NULL
};

/* Define objfmt structure -- see objfmt.h for details */
yasm_objfmt_module yasm_elf_LTX_objfmt = {
    "ELF",
    "elf",
    "o",
    32,
    elf_objfmt_dbgfmt_keywords,
    "null",
    elf_objfmt_create,
    elf_objfmt_output,
    elf_objfmt_destroy,
    elf_objfmt_add_default_section,
    elf_objfmt_section_switch,
    elf_objfmt_extern_declare,
    elf_objfmt_global_declare,
    elf_objfmt_common_declare,
    elf_objfmt_directive
};

yasm_objfmt_module yasm_elf32_LTX_objfmt = {
    "ELF (32-bit)",
    "elf32",
    "o",
    32,
    elf_objfmt_dbgfmt_keywords,
    "null",
    elf32_objfmt_create,
    elf_objfmt_output,
    elf_objfmt_destroy,
    elf_objfmt_add_default_section,
    elf_objfmt_section_switch,
    elf_objfmt_extern_declare,
    elf_objfmt_global_declare,
    elf_objfmt_common_declare,
    elf_objfmt_directive
};

yasm_objfmt_module yasm_elf64_LTX_objfmt = {
    "ELF (64-bit)",
    "elf64",
    "o",
    64,
    elf_objfmt_dbgfmt_keywords,
    "null",
    elf64_objfmt_create,
    elf_objfmt_output,
    elf_objfmt_destroy,
    elf_objfmt_add_default_section,
    elf_objfmt_section_switch,
    elf_objfmt_extern_declare,
    elf_objfmt_global_declare,
    elf_objfmt_common_declare,
    elf_objfmt_directive
};
